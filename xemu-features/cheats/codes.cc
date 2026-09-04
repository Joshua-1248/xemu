//
// xemu User Interface - Codes (cheats and patches)
//
// Layout mirrors MainMenuAudioView: two SectionTitle() dividers, "Cheats"
// where "Volume" sits and "Patches" where "Quality" sits.
//
// The engine and parser this drives are verified differentially against the
// original Python implementation; this file is the presentation layer over
// them.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "ui/xui/common.hh"
#include "ui/xui/widgets.hh"
#include "codes.hh"
#include "frontend.hh"
#include "debug-bridge.hh"
#include "xemu-features/scripting/frontend.hh"
#include "xemu-features/shared/detachable-windows.hh"
#include "xemu-features/shared/misc-menu.hh"
#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"
#include "xemu-xbe.h"
#include "xemu-features/shared/guest-memory.h"

#include <glib/gstdio.h>

#include <cstdio>
#include <cinttypes>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

CodesManager g_codes;

// ---------------------------------------------------------------------------
// Feature-only legacy Settings > Codes sidebar removal
// ---------------------------------------------------------------------------
//
// MainMenuScene intentionally keeps its implementation details protected, and
// native Xemu source is off-limits for custom-fork feature work.  Do not use a
// preprocessor #undef of CONFIG_XEMU_FEATURE_CHEATS here: main-menu.hh has
// already declared the Codes members by the time the feature frontend headers
// are included by main-menu.cc, so changing that define mid-translation-unit
// leaves a MainMenuTabButton member without its required constructor.
//
// These tiny pointer-to-member tags obtain the already-declared protected
// members without modifying or redefining the native class.  Access control
// does not affect object layout, and the resulting pointers are ordinary C++
// member pointers.  The feature then removes only its legacy settings tab from
// the two native vectors on first use.  No native Xemu/QEMU file is changed.
namespace feature_codes_settings_bridge {

template<class Tag, typename Tag::type Member>
struct ProtectedMemberAccess {
    friend typename Tag::type get_member(Tag) { return Member; }
};

struct TabsTag {
    using type = std::vector<MainMenuTabButton *> MainMenuScene::*;
    friend type get_member(TabsTag);
};
template struct ProtectedMemberAccess<TabsTag, &MainMenuScene::m_tabs>;

struct ViewsTag {
    using type = std::vector<MainMenuTabView *> MainMenuScene::*;
    friend type get_member(ViewsTag);
};
template struct ProtectedMemberAccess<ViewsTag, &MainMenuScene::m_views>;

struct CurrentIndexTag {
    using type = int MainMenuScene::*;
    friend type get_member(CurrentIndexTag);
};
template struct ProtectedMemberAccess<CurrentIndexTag,
                                      &MainMenuScene::m_current_view_index>;

struct NextIndexTag {
    using type = int MainMenuScene::*;
    friend type get_member(NextIndexTag);
};
template struct ProtectedMemberAccess<NextIndexTag,
                                      &MainMenuScene::m_next_view_index>;

struct CodesButtonTag {
    using type = MainMenuTabButton MainMenuScene::*;
    friend type get_member(CodesButtonTag);
};
template struct ProtectedMemberAccess<CodesButtonTag,
                                      &MainMenuScene::m_codes_button>;

struct CodesViewTag {
    using type = MainMenuCodesView MainMenuScene::*;
    friend type get_member(CodesViewTag);
};
template struct ProtectedMemberAccess<CodesViewTag,
                                      &MainMenuScene::m_codes_view>;

static void HideLegacySettingsTab()
{
    auto &tabs = g_main_menu.*get_member(TabsTag{});
    auto &views = g_main_menu.*get_member(ViewsTag{});
    auto *codes_button = &(g_main_menu.*get_member(CodesButtonTag{}));
    auto *codes_view = &(g_main_menu.*get_member(CodesViewTag{}));

    auto tab_it = std::find(tabs.begin(), tabs.end(), codes_button);
    if (tab_it == tabs.end()) {
        return; // already removed
    }

    const size_t index = (size_t)std::distance(tabs.begin(), tab_it);
    tabs.erase(tab_it);

    // The native Settings scene keeps m_tabs and m_views index-aligned.  Use
    // the corresponding slot when it is the expected Codes view; otherwise
    // fall back to finding that exact view pointer defensively.
    if (index < views.size() && views[index] == codes_view) {
        views.erase(views.begin() + (ptrdiff_t)index);
    } else {
        auto view_it = std::find(views.begin(), views.end(), codes_view);
        if (view_it != views.end()) {
            views.erase(view_it);
        }
    }

    auto &current = g_main_menu.*get_member(CurrentIndexTag{});
    auto &next = g_main_menu.*get_member(NextIndexTag{});
    const int removed = (int)index;

    // If the legacy Codes page happened to be active, return to General.
    // Otherwise preserve the user's active page while accounting for the
    // removed slot.
    if (current == removed) current = 0;
    else if (current > removed) --current;
    if (next == removed) next = 0;
    else if (next > removed) --next;
}

} // namespace feature_codes_settings_bridge

// ---------------------------------------------------------------------------
// Built-in cheat / patch text editor
// ---------------------------------------------------------------------------
// Work directly on the same text representation used on disk. This keeps the
// editor lossless with respect to the existing parser/writer and lets users
// add, remove, rename, regroup, and change codes without leaving xemu.
namespace {

struct CodesEditorState {
    CodesManager::Section *section = nullptr;
    std::string label;
    std::string stem;
    std::string text;
    std::string error;
    bool open_requested = false;
};

CodesEditorState g_codes_editor;

enum class NodeEditorMode {
    None,
    AddCheat,
    EditCheat,
    AddGroup,
    EditGroup,
};

struct NodeEditorState {
    NodeEditorMode mode = NodeEditorMode::None;
    CodesManager::Section *section = nullptr;
    xcheat::NodeList *parent = nullptr;
    xcheat::Node *node = nullptr;
    std::string stem;
    std::string title;
    std::string name;
    std::string author;
    std::string desc;
    std::string codes;
    std::string group_path;
    std::string error;
    bool enabled = false;
    bool open_requested = false;
    /* True only after an attempted tree mutation failed primary persistence.
     * The edited in-memory tree is intentionally kept so Save can be retried,
     * but Cancel must then restore the last verified disk-backed tree. */
    bool failed_save_dirty = false;
};

struct NodeActionState {
    enum class Kind { None, Duplicate, Delete } kind = Kind::None;
    CodesManager::Section *section = nullptr;
    xcheat::NodeList *parent = nullptr;
    xcheat::Node *node = nullptr;
    std::string stem;
    std::string name;
    bool open_delete_requested = false;
};

NodeEditorState g_node_editor;
NodeActionState g_node_action;

static bool g_codes_window_open = false;
static constexpr const char *kCodesDetachId = "cheats_patches";

static std::string TrimEditorText(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string LowerEditorText(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

static bool ParseEditorCodeLines(const std::string &text,
                                 std::vector<xcheat::Code> *out,
                                 std::string *error)
{
    out->clear();
    if (error) error->clear();

    size_t pos = 0;
    int lineno = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        ++lineno;
        line = TrimEditorText(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        size_t ws = line.find_first_of(" \t");
        if (ws == std::string::npos) {
            if (error) *error = "Line " + std::to_string(lineno) +
                ": expected COMMAND VALUE.";
            return false;
        }
        std::string a = line.substr(0, ws);
        size_t bpos = line.find_first_not_of(" \t", ws);
        if (bpos == std::string::npos) {
            if (error) *error = "Line " + std::to_string(lineno) +
                ": missing value.";
            return false;
        }
        size_t bend = line.find_first_of(" \t", bpos);
        std::string b = line.substr(bpos, bend == std::string::npos
                                                ? std::string::npos
                                                : bend - bpos);
        if (bend != std::string::npos &&
            !TrimEditorText(line.substr(bend)).empty()) {
            if (error) *error = "Line " + std::to_string(lineno) +
                ": expected exactly two hexadecimal values.";
            return false;
        }
        auto all_hex = [](const std::string &v) {
            if (v.empty() || v.size() > 8) return false;
            return std::all_of(v.begin(), v.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            });
        };
        if (!all_hex(a) || !all_hex(b)) {
            if (error) *error = "Line " + std::to_string(lineno) +
                ": code values must be 1-8 hexadecimal digits.";
            return false;
        }
        out->push_back({(uint32_t)strtoul(a.c_str(), nullptr, 16),
                        (uint32_t)strtoul(b.c_str(), nullptr, 16)});
    }

    if (out->empty()) {
        if (error) *error = "Enter at least one valid code line.";
        return false;
    }
    return true;
}

static std::string RenderEditorCodeLines(const std::vector<xcheat::Code> &codes)
{
    std::string out;
    char buf[32];
    for (const auto &c : codes) {
        snprintf(buf, sizeof(buf), "%08X %08X\n", c.cmd, c.val);
        out += buf;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

static std::string EscapeEditorPathComponent(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out += '\\';
        out += c;
    }
    return out;
}

static std::string JoinEditorPath(const std::vector<std::string> &parts)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '\\';
        out += EscapeEditorPathComponent(parts[i]);
    }
    return out;
}

static bool FindEditorListPath(xcheat::NodeList &nodes,
                               xcheat::NodeList *target,
                               std::vector<std::string> *path)
{
    if (&nodes == target) return true;
    for (auto &n : nodes) {
        if (!n->is_group) continue;
        path->push_back(n->name);
        if (&n->children == target ||
            FindEditorListPath(n->children, target, path)) {
            return true;
        }
        path->pop_back();
    }
    return false;
}

static std::string EditorPathForList(CodesManager::Section &sec,
                                     xcheat::NodeList *target)
{
    if (!target || target == &sec.root) return "";
    std::vector<std::string> path;
    if (!FindEditorListPath(sec.root, target, &path)) return "";
    return JoinEditorPath(path);
}

static void CollectEditorGroupPaths(xcheat::NodeList &nodes,
                                    std::vector<std::string> path,
                                    std::vector<std::string> *out)
{
    for (auto &n : nodes) {
        if (!n->is_group) continue;
        path.push_back(n->name);
        out->push_back(JoinEditorPath(path));
        CollectEditorGroupPaths(n->children, path, out);
        path.pop_back();
    }
}

static xcheat::NodeList *EnsureEditorGroupPath(CodesManager::Section &sec,
                                               const std::string &text)
{
    std::string clean = TrimEditorText(text);
    if (clean.empty()) return &sec.root;

    std::vector<std::string> parts = xcheat::SplitPath(clean, false);
    xcheat::NodeList *parent = &sec.root;
    for (const auto &part : parts) {
        xcheat::Node *found = nullptr;
        for (auto &n : *parent) {
            if (n->is_group &&
                LowerEditorText(n->name) == LowerEditorText(part)) {
                found = n.get();
                break;
            }
        }
        if (!found) {
            auto g = std::make_unique<xcheat::Node>();
            g->is_group = true;
            g->name = part;
            g->expanded = true;
            found = g.get();
            parent->push_back(std::move(g));
        }
        parent = &found->children;
    }
    return parent;
}

static bool MoveEditorNode(xcheat::NodeList *from, xcheat::NodeList *to,
                           xcheat::Node *node)
{
    if (!from || !to || !node || from == to) return true;
    auto it = std::find_if(from->begin(), from->end(),
                           [node](const auto &p) { return p.get() == node; });
    if (it == from->end()) return false;
    std::unique_ptr<xcheat::Node> held = std::move(*it);
    from->erase(it);
    to->push_back(std::move(held));
    return true;
}

static std::unique_ptr<xcheat::Node> CloneEditorNode(const xcheat::Node &src)
{
    auto out = std::make_unique<xcheat::Node>();
    out->is_group = src.is_group;
    out->name = src.name;
    out->desc = src.desc;
    out->author = src.author;
    out->codes = src.codes;
    // Duplicates start disabled. This avoids immediately running two copies of
    // a freeze or, worse, applying two independently-journalled [ASM] patches.
    out->enabled = false;
    out->expanded = src.expanded;
    if (src.is_group) {
        for (const auto &child : src.children) {
            out->children.push_back(CloneEditorNode(*child));
        }
    }
    return out;
}

static bool EditorGroupNameExists(const xcheat::NodeList &parent,
                                  const std::string &name,
                                  const xcheat::Node *except = nullptr)
{
    std::string low = LowerEditorText(TrimEditorText(name));
    return std::any_of(parent.begin(), parent.end(), [&](const auto &n) {
        return n.get() != except && n->is_group &&
               LowerEditorText(n->name) == low;
    });
}

static bool EditorHasAsmSuffix(const std::string &name)
{
    std::string s = TrimEditorText(name);
    if (s.size() < 5) return false;
    std::string tail = s.substr(s.size() - 5);
    return LowerEditorText(tail) == "[asm]";
}

static std::string UniqueEditorSiblingName(const xcheat::NodeList &parent,
                                           const std::string &source,
                                           bool preserve_asm_suffix)
{
    std::string base = TrimEditorText(source);
    if (base.empty()) base = "Copy";

    bool asm_suffix = preserve_asm_suffix && EditorHasAsmSuffix(base);
    if (asm_suffix) {
        base.erase(base.size() - 5);
        base = TrimEditorText(base);
        if (base.empty()) base = "Patch";
    }

    auto make_name = [&](int number) {
        std::string out = base + (number <= 1 ? " (Copy)"
                                             : " (Copy " + std::to_string(number) + ")");
        if (asm_suffix) out += " [ASM]";
        return out;
    };

    std::string candidate = make_name(1);
    int suffix = 2;
    auto exists = [&](const std::string &name) {
        std::string low = LowerEditorText(name);
        return std::any_of(parent.begin(), parent.end(), [&](const auto &n) {
            return LowerEditorText(n->name) == low;
        });
    };
    while (exists(candidate)) {
        candidate = make_name(suffix++);
    }
    return candidate;
}

static void OpenCheatNodeEditor(CodesManager::Section &sec,
                                xcheat::NodeList *parent,
                                xcheat::Node *node)
{
    g_node_editor = NodeEditorState{};
    g_node_editor.mode = node ? NodeEditorMode::EditCheat
                              : NodeEditorMode::AddCheat;
    g_node_editor.section = &sec;
    g_node_editor.parent = parent ? parent : &sec.root;
    g_node_editor.node = node;
    g_node_editor.stem = g_codes.Stem();
    bool patch = (&sec == &g_codes.Patches());
    g_node_editor.title = node ? (patch ? "Edit patch" : "Edit cheat")
                               : (patch ? "Add patch" : "Add cheat");
    g_node_editor.group_path = EditorPathForList(sec, g_node_editor.parent);
    if (node) {
        g_node_editor.name = node->name;
        g_node_editor.author = node->author;
        g_node_editor.desc = node->desc;
        g_node_editor.codes = RenderEditorCodeLines(node->codes);
        g_node_editor.enabled = node->enabled;
    }
    g_node_editor.open_requested = true;
}

static void OpenGroupNodeEditor(CodesManager::Section &sec,
                                xcheat::NodeList *parent,
                                xcheat::Node *node)
{
    g_node_editor = NodeEditorState{};
    g_node_editor.mode = node ? NodeEditorMode::EditGroup
                              : NodeEditorMode::AddGroup;
    g_node_editor.section = &sec;
    g_node_editor.parent = parent ? parent : &sec.root;
    g_node_editor.node = node;
    g_node_editor.stem = g_codes.Stem();
    g_node_editor.title = node ? "Rename group" : "Add group";
    if (node) g_node_editor.name = node->name;
    g_node_editor.open_requested = true;
}

static void RequestDuplicateNode(CodesManager::Section &sec,
                                 xcheat::NodeList &parent,
                                 xcheat::Node &node)
{
    g_node_action = NodeActionState{};
    g_node_action.kind = NodeActionState::Kind::Duplicate;
    g_node_action.section = &sec;
    g_node_action.parent = &parent;
    g_node_action.node = &node;
    g_node_action.stem = g_codes.Stem();
    g_node_action.name = node.name;
}

static void RequestDeleteNode(CodesManager::Section &sec,
                              xcheat::NodeList &parent,
                              xcheat::Node &node)
{
    g_node_action = NodeActionState{};
    g_node_action.kind = NodeActionState::Kind::Delete;
    g_node_action.section = &sec;
    g_node_action.parent = &parent;
    g_node_action.node = &node;
    g_node_action.stem = g_codes.Stem();
    g_node_action.name = node.name;
    g_node_action.open_delete_requested = true;
}

static void DrawNodeContextMenu(CodesManager::Section &sec,
                                xcheat::NodeList &parent,
                                xcheat::Node &node)
{
    if (!ImGui::BeginPopupContextItem("###xemu_code_node_context")) return;

    if (node.is_group) {
        if (ImGui::MenuItem("Add cheat here...")) {
            OpenCheatNodeEditor(sec, &node.children, nullptr);
        }
        if (ImGui::MenuItem("Add group here...")) {
            OpenGroupNodeEditor(sec, &node.children, nullptr);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename group...")) {
            OpenGroupNodeEditor(sec, &parent, &node);
        }
        if (ImGui::MenuItem("Duplicate group")) {
            RequestDuplicateNode(sec, parent, node);
        }
        if (ImGui::MenuItem("Delete group...")) {
            RequestDeleteNode(sec, parent, node);
        }
    } else {
        bool patch = (&sec == &g_codes.Patches());
        if (ImGui::MenuItem(patch ? "Edit patch..." : "Edit cheat...")) {
            OpenCheatNodeEditor(sec, &parent, &node);
        }
        if (ImGui::MenuItem(patch ? "Duplicate patch" : "Duplicate cheat")) {
            RequestDuplicateNode(sec, parent, node);
        }
        if (ImGui::MenuItem(patch ? "Delete patch..." : "Delete cheat...")) {
            RequestDeleteNode(sec, parent, node);
        }
    }
    ImGui::EndPopup();
}

void OpenCodesEditor(CodesManager::Section &sec, const char *label)
{
    if (g_codes.Stem().empty()) {
        return;
    }

    g_codes_editor.section = &sec;
    g_codes_editor.label = label ? label : "Codes";
    g_codes_editor.stem = g_codes.Stem();
    g_codes_editor.error.clear();
    g_codes_editor.text = xcheat::RenderCheatText(
        sec.meta.game.empty() ? g_codes.Title() : sec.meta.game,
        sec.meta.serial, sec.meta.titleid, sec.root, g_codes.Stem());
    g_codes_editor.open_requested = true;
}

void DrawCodesEditor()
{
    if (g_codes_editor.open_requested) {
        ImGui::OpenPopup("Code file editor###xemu_codes_editor");
        g_codes_editor.open_requested = false;
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Code file editor###xemu_codes_editor",
                                nullptr)) {
        return;
    }

    ImGui::Text("%s - %s", g_codes_editor.label.c_str(),
                g_codes_editor.stem.c_str());
    ImGui::TextWrapped(
        "Edit the code file directly. Save and reload validates the text "
        "before it is written and activated.");

    ImVec2 editor_size(ImGui::GetContentRegionAvail().x, 360.0f);
    ImGui::InputTextMultiline("###xemu_codes_text", &g_codes_editor.text,
                              editor_size,
                              ImGuiInputTextFlags_AllowTabInput);

    if (!g_codes_editor.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", g_codes_editor.error.c_str());
        ImGui::PopStyleColor();
    }

    if (ImGui::Button("Save and reload")) {
        g_codes_editor.error.clear();

        if (g_codes_editor.section == nullptr ||
            g_codes_editor.stem != g_codes.Stem()) {
            g_codes_editor.error =
                "The running game changed while the editor was open. "
                "Close this editor and reopen it for the current game.";
        } else {
            xcheat::NodeList parsed_root;
            xcheat::Meta parsed_meta;
            xcheat::ParseCheatText(g_codes_editor.text,
                                   &parsed_root, &parsed_meta);

            if (!parsed_meta.warnings.empty()) {
                g_codes_editor.error =
                    "Fix these parser warnings before saving:\n";
                for (const auto &warning : parsed_meta.warnings) {
                    g_codes_editor.error += "- " + warning + "\n";
                }
            } else {
                CodesManager::Section *sec = g_codes_editor.section;
                CodesManager::Section candidate;
                candidate.root = std::move(parsed_root);
                candidate.meta = std::move(parsed_meta);

                // Persist and verify the candidate before touching the live
                // tree. If persistence fails, the running tree and editor text
                // are both left intact and the emergency recovery path is
                // reported by SaveReplacement().
                std::string save_error;
                std::string save_path;
                if (g_codes.SaveReplacement(*sec, candidate, &save_error,
                                            &save_path)) {
                    g_codes.LoadForCurrentGame();
                    std::string msg = "Code file saved and verified: " + save_path;
                    xemu_queue_notification(msg.c_str());
                    ImGui::CloseCurrentPopup();
                    g_codes_editor = CodesEditorState{};
                } else {
                    g_codes_editor.error = save_error;
                }
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
        g_codes_editor = CodesEditorState{};
    }

    ImGui::EndPopup();
}

static void DrawNodeEditor()
{
    if (g_node_editor.open_requested) {
        ImGui::OpenPopup("Cheat entry editor###xemu_code_node_editor");
        g_node_editor.open_requested = false;
    }

    ImGui::SetNextWindowSize(ImVec2(620.0f, 650.0f), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal("Cheat entry editor###xemu_code_node_editor",
                               nullptr)) {
        if (!g_node_editor.title.empty()) {
            ImGui::TextUnformatted(g_node_editor.title.c_str());
            ImGui::Separator();
        }

        bool game_changed = g_node_editor.section == nullptr ||
                            g_node_editor.stem != g_codes.Stem();
        if (game_changed) {
            ImGui::TextWrapped(
                "The running game changed while this editor was open. "
                "Close it and reopen it for the current game.");
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("Name", &g_node_editor.name);

        bool is_cheat = g_node_editor.mode == NodeEditorMode::AddCheat ||
                        g_node_editor.mode == NodeEditorMode::EditCheat;
        if (is_cheat) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("Author", &g_node_editor.author);

            ImGui::TextUnformatted("Description");
            ImGui::InputTextMultiline("###xemu_code_desc", &g_node_editor.desc,
                                      ImVec2(-1.0f, 72.0f));

            ImGui::TextUnformatted("Group path");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("###xemu_code_group_path",
                             &g_node_editor.group_path);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Blank means top level. Use backslashes for nested groups, "
                    "for example Weapons\\Ammo. Missing groups are created.");
            }

            std::vector<std::string> paths;
            if (g_node_editor.section) {
                CollectEditorGroupPaths(g_node_editor.section->root, {}, &paths);
            }
            const char *preview = g_node_editor.group_path.empty()
                                      ? "(top level)"
                                      : g_node_editor.group_path.c_str();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("Existing groups", preview)) {
                bool top = g_node_editor.group_path.empty();
                if (ImGui::Selectable("(top level)", top)) {
                    g_node_editor.group_path.clear();
                }
                for (const auto &path : paths) {
                    bool selected = path == g_node_editor.group_path;
                    if (ImGui::Selectable(path.c_str(), selected)) {
                        g_node_editor.group_path = path;
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Checkbox("Enabled", &g_node_editor.enabled);

            ImGui::TextUnformatted(
                "Code lines (COMMAND VALUE, hexadecimal):");
            ImGui::InputTextMultiline(
                "###xemu_code_lines", &g_node_editor.codes,
                ImVec2(-1.0f, 220.0f), ImGuiInputTextFlags_AllowTabInput);
        }

        if (!g_node_editor.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("%s", g_node_editor.error.c_str());
            ImGui::PopStyleColor();
        }

        if (game_changed) ImGui::BeginDisabled();
        if (ImGui::Button("Save")) {
            g_node_editor.error.clear();
            std::string name = TrimEditorText(g_node_editor.name);
            if (name.empty()) {
                g_node_editor.error = "Name cannot be empty.";
            } else if ((g_node_editor.mode == NodeEditorMode::AddGroup ||
                        g_node_editor.mode == NodeEditorMode::EditGroup) &&
                       g_node_editor.parent &&
                       EditorGroupNameExists(*g_node_editor.parent, name,
                                             g_node_editor.node)) {
                g_node_editor.error =
                    "A group with that name already exists at this level.";
            } else if (!g_node_editor.section ||
                       g_node_editor.stem != g_codes.Stem()) {
                g_node_editor.error = "The running game changed.";
            } else if (is_cheat) {
                std::vector<xcheat::Code> parsed;
                if (ParseEditorCodeLines(g_node_editor.codes, &parsed,
                                         &g_node_editor.error)) {
                    CodesManager::Section &sec = *g_node_editor.section;
                    xcheat::NodeList *target =
                        EnsureEditorGroupPath(sec, g_node_editor.group_path);
                    xcheat::Node *changed = nullptr;

                    if (g_node_editor.mode == NodeEditorMode::AddCheat) {
                        auto n = std::make_unique<xcheat::Node>();
                        n->is_group = false;
                        n->name = name;
                        n->author = TrimEditorText(g_node_editor.author);
                        n->desc = TrimEditorText(g_node_editor.desc);
                        n->codes = std::move(parsed);
                        n->enabled = g_node_editor.enabled;
                        changed = n.get();
                        target->push_back(std::move(n));
                    } else if (g_node_editor.node && g_node_editor.parent) {
                        changed = g_node_editor.node;
                        g_codes.PrepareNodeMutation(*changed);
                        changed->name = name;
                        changed->author = TrimEditorText(g_node_editor.author);
                        changed->desc = TrimEditorText(g_node_editor.desc);
                        changed->codes = std::move(parsed);
                        changed->enabled = g_node_editor.enabled;
                        if (!MoveEditorNode(g_node_editor.parent, target,
                                            changed)) {
                            g_node_editor.error =
                                "The cheat moved while the editor was open.";
                            changed = nullptr;
                        }
                    }

                    if (changed) {
                        std::string save_error;
                        std::string save_path;
                        if (g_codes.FinishTreeMutation(
                                sec, changed, &save_error, &save_path)) {
                            std::string msg = "Cheat file saved and verified: " +
                                              save_path;
                            xemu_queue_notification(msg.c_str());
                            ImGui::CloseCurrentPopup();
                            g_node_editor = NodeEditorState{};
                        } else {
                            // The edited node remains in memory so the user's
                            // work is not destroyed. Rebind the editor to its
                            // current location so pressing Save again retries
                            // the same node instead of creating a duplicate.
                            g_node_editor.error = save_error;
                            g_node_editor.mode = NodeEditorMode::EditCheat;
                            g_node_editor.parent = target;
                            g_node_editor.node = changed;
                            g_node_editor.group_path =
                                EditorPathForList(sec, target);
                            g_node_editor.failed_save_dirty = true;
                        }
                    }
                }
            } else {
                if (g_node_editor.mode == NodeEditorMode::AddGroup) {
                    auto g = std::make_unique<xcheat::Node>();
                    g->is_group = true;
                    g->name = name;
                    g->expanded = true;
                    xcheat::Node *created = g.get();
                    xcheat::NodeList *parent =
                        g_node_editor.parent ? g_node_editor.parent
                                             : &g_node_editor.section->root;
                    parent->push_back(std::move(g));
                    std::string save_error;
                    std::string save_path;
                    if (g_codes.FinishTreeMutation(*g_node_editor.section,
                                                   created, &save_error,
                                                   &save_path)) {
                        std::string msg = "Code file saved and verified: " +
                                          save_path;
                        xemu_queue_notification(msg.c_str());
                        ImGui::CloseCurrentPopup();
                        g_node_editor = NodeEditorState{};
                    } else {
                        g_node_editor.error = save_error;
                        g_node_editor.mode = NodeEditorMode::EditGroup;
                        g_node_editor.parent = parent;
                        g_node_editor.node = created;
                        g_node_editor.failed_save_dirty = true;
                    }
                } else if (g_node_editor.node && g_node_editor.node->is_group) {
                    g_node_editor.node->name = name;
                    std::string save_error;
                    std::string save_path;
                    if (g_codes.FinishTreeMutation(
                            *g_node_editor.section, g_node_editor.node,
                            &save_error, &save_path)) {
                        std::string msg = "Code file saved and verified: " +
                                          save_path;
                        xemu_queue_notification(msg.c_str());
                        ImGui::CloseCurrentPopup();
                        g_node_editor = NodeEditorState{};
                    } else {
                        g_node_editor.error = save_error;
                        g_node_editor.failed_save_dirty = true;
                    }
                }
            }
        }

        if (game_changed) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (g_node_editor.failed_save_dirty) {
                /* The editor was deliberately holding an unsaved candidate so
                 * the user could retry. Cancel means abandon that candidate;
                 * reload the last verified primary instead of letting the
                 * unsaved tree linger in this session. */
                g_codes.LoadForCurrentGame();
            }
            ImGui::CloseCurrentPopup();
            g_node_editor = NodeEditorState{};
        }

        ImGui::EndPopup();
    }
}

static void ProcessNodeActions()
{
    if (g_node_action.kind == NodeActionState::Kind::Duplicate) {
        if (g_node_action.section && g_node_action.parent &&
            g_node_action.node && g_node_action.stem == g_codes.Stem()) {
            auto it = std::find_if(
                g_node_action.parent->begin(), g_node_action.parent->end(),
                [&](const auto &p) { return p.get() == g_node_action.node; });
            if (it != g_node_action.parent->end()) {
                auto clone = CloneEditorNode(**it);
                clone->name = UniqueEditorSiblingName(
                    *g_node_action.parent, clone->name, !clone->is_group);
                auto inserted = g_node_action.parent->insert(it + 1,
                                                              std::move(clone));
                std::string save_error;
                std::string save_path;
                if (g_codes.FinishTreeMutation(*g_node_action.section, nullptr,
                                               &save_error, &save_path)) {
                    std::string msg = "Cheat entry duplicated and saved: " +
                                      save_path;
                    xemu_queue_notification(msg.c_str());
                } else {
                    /* Duplicate is not an editor workflow, so roll the failed
                     * candidate back immediately. */
                    g_node_action.parent->erase(inserted);
                }
            }
        }
        g_node_action = NodeActionState{};
    }

    if (g_node_action.open_delete_requested) {
        ImGui::OpenPopup("Delete cheat entry?###xemu_code_delete_confirm");
        g_node_action.open_delete_requested = false;
    }

    if (ImGui::BeginPopupModal(
            "Delete cheat entry?###xemu_code_delete_confirm", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Delete '%s'%s?",
                           g_node_action.name.c_str(),
                           g_node_action.node && g_node_action.node->is_group
                               ? " and everything inside it"
                               : "");
        ImGui::TextDisabled("This rewrites the current game's .txt file.");

        if (ImGui::Button("Delete")) {
            if (g_node_action.section && g_node_action.parent &&
                g_node_action.node && g_node_action.stem == g_codes.Stem()) {
                auto it = std::find_if(
                    g_node_action.parent->begin(), g_node_action.parent->end(),
                    [&](const auto &p) { return p.get() == g_node_action.node; });
                if (it != g_node_action.parent->end()) {
                    size_t restore_index =
                        (size_t)std::distance(g_node_action.parent->begin(), it);
                    g_codes.PrepareNodeMutation(**it);
                    std::unique_ptr<xcheat::Node> removed = std::move(*it);
                    g_node_action.parent->erase(it);
                    std::string save_error;
                    std::string save_path;
                    if (g_codes.FinishTreeMutation(*g_node_action.section,
                                                   nullptr, &save_error,
                                                   &save_path)) {
                        std::string msg = "Code file saved and verified: " +
                                          save_path;
                        xemu_queue_notification(msg.c_str());
                    } else {
                        auto restore_at = g_node_action.parent->begin() +
                            std::min(restore_index,
                                     g_node_action.parent->size());
                        g_node_action.parent->insert(restore_at,
                                                     std::move(removed));
                    }
                }
            }
            ImGui::CloseCurrentPopup();
            g_node_action = NodeActionState{};
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            g_node_action = NodeActionState{};
        }
        ImGui::EndPopup();
    }
}

static void DrawIndividualEditors()
{
    DrawNodeEditor();
    ProcessNodeActions();
}

static void DrawStandaloneSection(CodesManager::Section &sec, bool patches)
{
    const char *kind = patches ? "patch" : "cheat";

    const bool no_game = g_codes.Stem().empty();
    if (no_game) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button(patches ? "Add patch..." : "Add cheat...")) {
        OpenCheatNodeEditor(sec, &sec.root, nullptr);
    }
    ImGui::SameLine();
    if (ImGui::Button(patches ? "Add group...##patch_window"
                              : "Add group...##cheat_window")) {
        OpenGroupNodeEditor(sec, &sec.root, nullptr);
    }
    ImGui::SameLine();
    if (ImGui::Button(patches ? "Edit entire .txt...##patch_window"
                              : "Edit entire .txt...##cheat_window")) {
        OpenCodesEditor(sec, patches ? "Patches" : "Cheats");
    }
    ImGui::SameLine();
    if (ImGui::Button(patches ? "Reload patches" : "Reload cheats")) {
        g_codes.LoadForCurrentGame();
    }

    if (no_game) {
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Right-click a %s or group to edit, duplicate, delete, or add inside it.",
                        kind);
    if (!sec.source_path.empty()) {
        ImGui::TextDisabled("Loaded: %s", sec.source_path.c_str());
    } else if (!no_game) {
        ImGui::TextDisabled("No file loaded. Expected filename: %s.txt",
                            g_codes.Stem().c_str());
    }

    const char *empty_msg = nullptr;
    if (sec.source_path.empty()) {
        empty_msg = patches ? "No patch file found for this game."
                            : "No cheat file found for this game.";
    } else {
        empty_msg = patches ? "Patch file loaded, but it contains no entries."
                            : "Cheat file loaded, but it contains no entries.";
    }
    g_codes.DrawSection(sec, empty_msg);

}

static void DrawCheatsPatchesWindow()
{
    xemu_feature_detach::Register(kCodesDetachId, "Cheats/Patches",
                                  &g_codes_window_open,
                                  []() { DrawCheatsPatchesWindow(); });
    xemu_feature_detach::Pump();

    if (!g_codes_window_open ||
        !xemu_feature_detach::ShouldDraw(kCodesDetachId)) {
        return;
    }

    if (xemu_feature_detach::IsDetachedPass(kCodesDetachId)) {
        xemu_feature_detach::PrepareWindow(kCodesDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f), ImGuiCond_FirstUseEver);
    }

    ImGuiWindowFlags flags = xemu_feature_detach::WindowFlags(kCodesDetachId, 0);
    if (!ImGui::Begin("Cheats/Patches", &g_codes_window_open, flags)) {
        xemu_feature_detach::ObserveCurrentWindow(kCodesDetachId);
        ImGui::End();
        return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kCodesDetachId);

    // Do not let a stale Dear ImGui gamepad-navigation target keep toggling
    // cheats after the user has returned focus to the game or another tool.
    // Keep the normal visual appearance while disabling item interaction.
    // Clicking the window focuses it; its controls become interactive on the
    // following frame, while controller input cannot make accidental changes
    // in an unfocused Cheats/Patches window.
    const bool codes_window_focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        xemu_feature_detach::HasNativeInputFocus(kCodesDetachId);
    if (!codes_window_focused) {
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
        ImGui::BeginDisabled();
    }

    if (g_codes.Stem().empty()) {
        ImGui::TextDisabled("No game running.");
    } else {
        ImGui::Text("%s", g_codes.Title().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", g_codes.Stem().c_str());
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("Enable codes", &g_config.codes.enable)) {
        xemu_settings_save();
    }

    int interval = g_config.codes.interval_ms;
    interval = std::clamp(interval, 0, 1000);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::SliderInt("Apply ms", &interval, 0, 1000,
                         interval == 0 ? "Instant" : "%d ms")) {
        g_config.codes.interval_ms = interval;
    }
    // Persist once when the drag/edit completes instead of rewriting xemu.toml
    // for every intermediate slider value.
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        xemu_settings_save();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Cheat/Patch Files");
    FilePicker("Cheats directory", g_config.codes.cheats_dir, nullptr, 0, true,
               [](const char *path) {
                   xemu_settings_set_string(&g_config.codes.cheats_dir, path);
                   // Persist the directory choice immediately. Otherwise a
                   // crash/force-close can leave perfectly good cheat files in
                   // the custom folder while the next launch silently falls
                   // back to the default folder and makes them look lost.
                   xemu_settings_save();
                   g_codes.LoadForCurrentGame();
               });
    FilePicker("Patches directory", g_config.codes.patches_dir, nullptr, 0, true,
               [](const char *path) {
                   xemu_settings_set_string(&g_config.codes.patches_dir, path);
                   xemu_settings_save();
                   g_codes.LoadForCurrentGame();
               });
    ImGui::TextDisabled(
        "Each picker may point directly at its folder, or both may point at "
        "a database root containing cheats/ and patches/.");
    ImGui::TextDisabled(
        "Cheat edits save immediately, are read-back verified, and keep a .bak of the previous file.");

    ImGui::Separator();
    if (ImGui::BeginTabBar("##cheats_patches_tabs")) {
        if (ImGui::BeginTabItem("Cheats")) {
            DrawStandaloneSection(g_codes.Cheats(), false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Patches")) {
            DrawStandaloneSection(g_codes.Patches(), true);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (!codes_window_focused) {
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }

    // Editors/modals are intentionally outside the root-window focus gate:
    // when one of them owns focus, it must remain fully interactive.
    DrawCodesEditor();
    DrawIndividualEditors();
    ImGui::End();
}

} // anonymous namespace

void FeatureCodesOpenWindow()
{
    // Populate the tree immediately when opened instead of waiting for the
    // next periodic runtime poll. This works even with Enable codes unchecked.
    g_codes.IdentifyGame();
    g_codes_window_open = true;
}

void FeatureCodesDrawMiscMenuItem()
{
    feature_codes_settings_bridge::HideLegacySettingsTab();
    if (ImGui::MenuItem("Cheats/Patches", nullptr, g_codes_window_open)) {
        FeatureCodesOpenWindow();
    }
}

void FeatureCodesShowWindows()
{
    feature_codes_settings_bridge::HideLegacySettingsTab();
    DrawCheatsPatchesWindow();
}

bool FeatureCodesWindowsOpen()
{
    return g_codes_window_open;
}

#ifndef CONFIG_XEMU_FEATURE_SCRIPTING
// The native menubar/main loop already calls these three Misc aggregation
// hooks. When Scripting is compiled out, Cheats supplies the implementation so
// the rest of the custom tool windows remain independently usable without
// adding a native Xemu integration point.
void FeatureScriptToolsDrawMenu()
{
    if (ImGui::BeginMenu("Misc")) {
        FeatureCodesDrawMiscMenuItem();
        FeatureCustomToolsDrawMiscMenuItems();
        ImGui::EndMenu();
    }
}

void FeatureScriptToolsShowWindows()
{
    FeatureCodesShowWindows();
    FeatureCustomToolsShowWindows();
}

bool FeatureScriptToolsWindowsOpen()
{
    return FeatureCodesWindowsOpen() || FeatureCustomToolsWindowsOpen();
}
#endif

// ---------------------------------------------------------------------------
// GuestMemory - the bridge between the interpreter and the machine
// ---------------------------------------------------------------------------
// The interpreter addresses RAM by PHYSICAL offset and never learns what is
// behind this class, which is what allowed it to be verified on the host
// against a flat buffer before any of xemu existed in the picture.

bool GuestMemory::Read(uint32_t off, void *buf, size_t len)
{
    return xemu_phys_read(off, buf, len) == (ssize_t)len;
}

size_t GuestMemory::ReadPartial(uint32_t off, void *buf, size_t len)
{
    // Only type 5 needs this, and only because the Python reference copies a
    // short read rather than failing. Clamp to RAM and read what is there.
    uint64_t ram = xemu_guest_ram_size();
    if (off >= ram) {
        return 0;
    }
    uint64_t avail = ram - off;
    size_t n = (len < avail) ? len : (size_t)avail;
    ssize_t r = xemu_phys_read(off, buf, n);
    return r < 0 ? 0 : (size_t)r;
}

void GuestMemory::Write(uint32_t off, const void *buf, size_t len)
{
    // [ASM] blocks capture the pre-patch bytes before the first write to each
    // physical byte. Ordinary data cheats never pay for this read/journal.
    JournalAsmWrite(off, len);

    // Failure is deliberately silent: a cheat aimed at an address the running
    // title has not mapped is a bad cheat, not an emulator fault, and warning
    // once per frame at 60 Hz would bury the log.
    xemu_phys_write(off, buf, len);
}

uint32_t GuestMemory::RamSize() const
{
    return (uint32_t)xemu_guest_ram_size();
}

void GuestMemory::BeginAsmBlock(uint32_t bid)
{
    m_asm_active = true;
    m_asm_bid = bid;
}

void GuestMemory::EndAsmBlock()
{
    m_asm_active = false;
    m_asm_bid = 0;
}

void GuestMemory::JournalAsmWrite(uint32_t off, size_t len)
{
    if (!m_asm_active || len == 0) {
        return;
    }

    const uint64_t range_key = ((uint64_t)off << 32) |
                               (uint32_t)std::min<size_t>(len, UINT32_MAX);
    auto &known_ranges = m_asm_journaled_ranges[m_asm_bid];
    if (len <= UINT32_MAX && known_ranges.find(range_key) != known_ranges.end()) {
        return;
    }

    auto &journal = m_asm_orig[m_asm_bid];
    bool need_read = false;
    for (size_t i = 0; i < len; ++i) {
        if (journal.find(off + (uint32_t)i) == journal.end()) {
            need_read = true;
            break;
        }
    }
    if (!need_read) {
        if (len <= UINT32_MAX) known_ranges.insert(range_key);
        return;
    }

    uint8_t stack_original[64];
    std::vector<uint8_t> heap_original;
    uint8_t *original = stack_original;
    if (len > sizeof(stack_original)) {
        heap_original.resize(len);
        original = heap_original.data();
    }
    if (xemu_phys_read(off, original, len) != (ssize_t)len) {
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        journal.emplace(off + (uint32_t)i, original[i]);
    }
    if (len <= UINT32_MAX) known_ranges.insert(range_key);
}

GuestMemory::AsmRestoreResult GuestMemory::RestoreAsm(uint32_t bid)
{
    AsmRestoreResult result;
    auto it = m_asm_orig.find(bid);
    if (it == m_asm_orig.end()) {
        return result;
    }

    std::vector<std::pair<uint32_t, uint8_t>> bytes;
    bytes.reserve(it->second.size());
    for (const auto &entry : it->second) {
        bytes.push_back(entry);
    }
    std::sort(bytes.begin(), bytes.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    size_t pos = 0;
    std::vector<uint8_t> run;
    run.reserve(std::min<size_t>(bytes.size(), 4096));
    while (pos < bytes.size()) {
        const uint32_t start = bytes[pos].first;
        run.clear();
        run.push_back(bytes[pos].second);
        uint32_t prev = start;
        ++pos;
        while (pos < bytes.size() && bytes[pos].first == prev + 1) {
            prev = bytes[pos].first;
            run.push_back(bytes[pos].second);
            ++pos;
        }
        if (xemu_phys_write(start, run.data(), run.size()) == (ssize_t)run.size()) {
            ++result.restored_ranges;
        } else {
            ++result.failed_ranges;
        }
    }

    // Never carry original bytes indefinitely after a restore attempt. If a
    // title has unloaded/replaced the mapping, retrying stale code later is
    // more dangerous than reporting the failed range once.
    m_asm_orig.erase(it);
    m_asm_journaled_ranges.erase(bid);
    return result;
}

GuestMemory::AsmRestoreResult GuestMemory::RestoreAllAsm()
{
    AsmRestoreResult total;
    std::vector<uint32_t> bids;
    bids.reserve(m_asm_orig.size());
    for (const auto &entry : m_asm_orig) {
        bids.push_back(entry.first);
    }
    for (uint32_t bid : bids) {
        AsmRestoreResult r = RestoreAsm(bid);
        total.restored_ranges += r.restored_ranges;
        total.failed_ranges += r.failed_ranges;
    }
    return total;
}

void GuestMemory::ClearAsmJournal()
{
    m_asm_orig.clear();
    m_asm_journaled_ranges.clear();
    EndAsmBlock();
}

size_t GuestMemory::AsmPatchByteCount(uint32_t bid, bool all) const
{
    if (!all) {
        auto it = m_asm_orig.find(bid);
        return it == m_asm_orig.end() ? 0 : it->second.size();
    }
    size_t total = 0;
    for (const auto &entry : m_asm_orig) {
        total += entry.second.size();
    }
    return total;
}

// ---------------------------------------------------------------------------
// Game identification
// ---------------------------------------------------------------------------
// No external database, no disc hashing. The XBE certificate carries the title
// id, and the serial is derivable from it arithmetically - 0x4D530064 is
// literally "MS" followed by game number 100. The filename stem the whole
// database is keyed on is SERIAL_TITLEID, so identification is a snprintf.

bool CodesManager::IdentifyGame()
{
    uint32_t tid = 0;
    if (!xemu_get_xbe_title_id(&tid)) {
        // Stop applying the old title immediately, but do not throw away its
        // [ASM] restore journal merely because one polling read failed. A
        // later successful identification decides whether the title really
        // changed.
        m_stem.clear();
        m_title.clear();
        m_live.clear();
        return false;
    }

    std::string serial;
    if (!xcheat::SerialFromTitleId(tid, &serial)) {
        m_stem.clear();
        m_title.clear();
        m_live.clear();
        return false;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%s_%08X", serial.c_str(), tid);
    std::string stem = buf;

    if (stem == m_stem) {
        return true;                    // unchanged, nothing to reload
    }

    // A journal is valid only for the title that produced it.  A transient
    // identification miss clears m_stem/m_live so stale cheats stop applying,
    // but m_asm_journal_stem survives that miss.  Only a positively identified
    // *different* title invalidates the original-byte journal.
    if (m_asm_journal_stem != stem) {
        m_mem.ClearAsmJournal();
        m_asm_journal_stem = stem;
    }

    /* The cheap query above avoids copying the whole XBE header every poll.
     * We only need the full certificate once, when the title actually changes,
     * to refresh the human-readable UTF-16 title string. Preserve the original
     * failure semantics: if that full read is transiently unavailable, do not
     * install a new game's code tree until identification is complete. */
    struct xbe *xbe = xemu_get_xbe_info();
    if (!xbe || !xbe->cert) {
        m_stem.clear();
        m_title.clear();
        m_live.clear();
        return false;
    }
    m_stem = stem;

    // m_title_name is a fixed uint16_t[40], NUL-PADDED rather than
    // NUL-terminated, so the length must be given explicitly (this is what
    // xemu-snapshots.c does too). Converting all 40 units yields a buffer
    // with the padding still in it; assigning to std::string stops at the
    // first NUL, which is what we want -- but a title that is entirely
    // padding then yields an empty string, so fall back to the stem rather
    // than showing a blank name in the settings pane.
    gchar *name = g_utf16_to_utf8(xbe->cert->m_title_name, 40,
                                  NULL, NULL, NULL);
    m_title = name ? std::string(name) : std::string();
    g_free(name);
    while (!m_title.empty() &&
           (m_title.back() == ' ' || m_title.back() == '\t')) {
        m_title.pop_back();
    }
    if (m_title.empty()) {
        m_title = stem;
    }

    LoadForCurrentGame();
    return true;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

// Cheats and patches get INDEPENDENT directories, mirroring the way
// texture_dump_dir and texture_replace_dir are separate rather than two
// subfolders of one root. A user-set path is used as-is, with no per-kind
// subfolder appended -- the setting already says which kind it is, and
// appending one would mean a folder full of cheat files could not simply be
// pointed at.
//
// Blank falls back to <xemu data dir>/codes/{cheats,patches}/, so the default
// layout is unchanged.
//
// Caller frees the result.
char *CodesManager::DirFor(const char *folder)
{
    bool is_patches = (strcmp(folder, "patches") == 0);
    const char *root = is_patches ? g_config.codes.patches_dir
                                  : g_config.codes.cheats_dir;
    if (root && root[0]) {
        return g_strdup(root);
    }
    return g_build_filename(xemu_settings_get_base_path(), "codes", folder,
                            NULL);
}

static bool CodesRegularFile(const std::string &path)
{
    return !path.empty() &&
           g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR);
}

static std::string CodesJoin(const std::string &a, const char *b)
{
    char *p = g_build_filename(a.c_str(), b, NULL);
    std::string out = p ? p : "";
    g_free(p);
    return out;
}

static std::string CodesJoin(const std::string &a, const std::string &b)
{
    return CodesJoin(a, b.c_str());
}

static std::string CodesFindCaseInsensitive(const std::string &dir,
                                            const std::string &wanted)
{
    GError *err = NULL;
    GDir *d = g_dir_open(dir.c_str(), 0, &err);
    if (!d) {
        if (err) g_error_free(err);
        return "";
    }

    std::string found;
    const char *name = NULL;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (g_ascii_strcasecmp(name, wanted.c_str()) != 0) {
            continue;
        }
        std::string path = CodesJoin(dir, name);
        if (CodesRegularFile(path)) {
            found = std::move(path);
            break;
        }
    }
    g_dir_close(d);
    return found;
}

static std::string CodesPreferredDirectory(const char *configured_dir,
                                           const char *folder)
{
    const std::string root = configured_dir ? configured_dir : "";
    std::string nested = CodesJoin(root, folder);

    // A selected database root from the standalone trainer has real cheats/
    // and patches/ children. Prefer that existing child for new files. A
    // direct per-kind selection has no same-name child and remains unchanged.
    if (g_file_test(nested.c_str(), G_FILE_TEST_IS_DIR)) {
        return nested;
    }
    return root;
}

static std::string CodesFindFile(const char *configured_dir,
                                 const char *folder,
                                 const std::string &stem)
{
    const std::string root = configured_dir ? configured_dir : "";
    const std::string wanted = stem + ".txt";

    // Accept both supported UI interpretations:
    //   1) the picker points directly at the cheats/patches directory;
    //   2) it points at a common parent containing cheats/ and patches/.
    // The first remains canonical and wins when both exist.
    std::vector<std::string> dirs;
    dirs.reserve(2);
    dirs.push_back(root);
    std::string nested = CodesJoin(root, folder);
    if (nested != root) {
        dirs.push_back(std::move(nested));
    }

    for (const std::string &dir : dirs) {
        std::string exact = CodesJoin(dir, wanted);
        if (CodesRegularFile(exact)) {
            return exact;
        }
    }

    // Linux filesystems are normally case-sensitive; existing trainer packs
    // created on Windows are not guaranteed to preserve the canonical case.
    for (const std::string &dir : dirs) {
        std::string found = CodesFindCaseInsensitive(dir, wanted);
        if (!found.empty()) {
            return found;
        }
    }

    return "";
}

void CodesManager::LoadOne(Section &sec, const char *folder)
{
    sec.root.clear();
    sec.meta = xcheat::Meta();
    sec.source_path.clear();
    if (m_stem.empty()) return;

    char *dir = CodesManager::DirFor(folder);
    std::string full = CodesFindFile(dir, folder, m_stem);

    if (full.empty()) {
        char *expected_name = g_strdup_printf("%s.txt", m_stem.c_str());
        char *expected = g_build_filename(dir, expected_name, NULL);
        fprintf(stderr,
                "[codes] no %s file for %s; expected %s "
                "(or a matching file in %s/)\n",
                folder, m_stem.c_str(), expected, folder);
        g_free(expected);
        g_free(expected_name);
        g_free(dir);
        return;
    }
    g_free(dir);

    gchar *contents = NULL;
    gsize len = 0;
    bool ok = g_file_get_contents(full.c_str(), &contents, &len, NULL);
    if (!ok) {
        fprintf(stderr, "[codes] could not read %s\n", full.c_str());
        return;
    }
    xcheat::ParseCheatText(std::string(contents, len), &sec.root, &sec.meta);
    g_free(contents);
    sec.source_path = full;

    fprintf(stderr, "[codes] loaded %s\n", full.c_str());

    // Warnings are printed, not swallowed. A hand-edited file with a typo
    // should say so somewhere findable.
    for (const auto &w : sec.meta.warnings) {
        fprintf(stderr, "[codes] %s: %s\n", m_stem.c_str(), w.c_str());
    }
}

void CodesManager::LoadForCurrentGame()
{
    // A same-title reload can replace node identities and code lines. Restore
    // every active [ASM] patch before throwing those identities away. On an
    // actual game change IdentifyGame() already cleared the stale journal.
    m_mem.RestoreAllAsm();
    LoadOne(m_cheats, "cheats");
    LoadOne(m_patches, "patches");
    m_engine.ClearSwitches(0, true);
    RebuildLive();
}

void CodesManager::RebuildLiveFrom(const xcheat::NodeList &nodes)
{
    for (const auto &n : nodes) {
        if (n->is_group) {
            RebuildLiveFrom(n->children);
            continue;
        }
        if (!n->enabled || n->codes.empty()) {
            continue;
        }

        CompiledBlock block;
        block.node = n.get();
        block.bid = (uint32_t)(uintptr_t)n.get();
        block.asm_patch = IsAsmName(n->name);
        block.codes.reserve(n->codes.size());
        for (const auto &c : n->codes) {
            block.codes.push_back({ c.cmd, c.val });
        }
        m_live.push_back(std::move(block));
    }
}

void CodesManager::RebuildLive()
{
    m_live.clear();
    RebuildLiveFrom(m_cheats.root);
    RebuildLiveFrom(m_patches.root);
}

bool CodesManager::IsAsmName(const std::string &name)
{
    size_t end = name.size();
    while (end && std::isspace((unsigned char)name[end - 1])) {
        --end;
    }
    static const char suffix[] = "[ASM]";
    if (end < sizeof(suffix) - 1) {
        return false;
    }
    size_t start = end - (sizeof(suffix) - 1);
    for (size_t i = 0; i < sizeof(suffix) - 1; ++i) {
        if (std::toupper((unsigned char)name[start + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

void CodesManager::ApplyBlockNow(const CompiledBlock &block)
{
    xemu_guestmem_invalidate_cache();
    if (!m_engine_attached) {
        m_engine.Attach(&m_mem);
        m_engine_attached = true;
    } else {
        m_engine.InvalidatePageMap();
    }

    if (block.asm_patch) {
        m_mem.BeginAsmBlock(block.bid);
    }
    m_engine.ExecuteBlock(block.codes, block.bid);
    if (block.asm_patch) {
        m_mem.EndAsmBlock();
    }
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------
// Written the moment a checkbox changes, NOT at exit.
//
// xemu.c installs atexit(xemu_settings_save), which does not run on a crash,
// a SIGKILL, or a force-close - exactly the cases where losing state is most
// annoying. Enabled flags live in the cheat files rather than the config, so
// they are saved here on the same principle: immediately and transactionally.
//
// Persistence rules for the custom fork:
//   1. write the complete new file to <path>.tmp;
//   2. read it back and byte-verify it before touching the live file;
//   3. preserve the previous live file as <path>.bak when one exists;
//   4. replace the live file, with a Windows-compatible rename fallback;
//   5. read the final live file back and byte-verify it;
//   6. if the primary destination fails, make a best-effort emergency copy
//      under <xemu data>/codes/recovery/ and report both paths to the user.
//
// Callers receive a real success/failure result. No UI path is allowed to say
// "saved" merely because the in-memory tree changed.

static bool FileContentsEqual(const char *path, const std::string &expected,
                              std::string *why)
{
    gchar *contents = nullptr;
    gsize len = 0;
    GError *err = nullptr;
    if (!g_file_get_contents(path, &contents, &len, &err)) {
        if (why) {
            *why = std::string("could not read back '") + path + "': " +
                   (err ? err->message : "unknown error");
        }
        if (err) g_error_free(err);
        return false;
    }

    bool same = len == expected.size() &&
                (len == 0 || std::memcmp(contents, expected.data(), len) == 0);
    g_free(contents);
    if (!same && why) {
        *why = std::string("read-back verification failed for '") + path +
               "' (file contents differ from the data xemu wrote)";
    }
    return same;
}

static bool WriteRecoveryCopy(const std::string &stem, const char *folder,
                              const std::string &text,
                              std::string *recovery_path)
{
    char *dir = g_build_filename(xemu_settings_get_base_path(), "codes",
                                 "recovery", NULL);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_free(dir);
        return false;
    }

    char *fname = g_strdup_printf("%s.%s.recovery.txt", stem.c_str(), folder);
    char *full = g_build_filename(dir, fname, NULL);

    g_free(fname);
    g_free(dir);

    GError *err = nullptr;
    bool ok = g_file_set_contents(full, text.data(), (gssize)text.size(), &err);
    if (err) g_error_free(err);
    if (ok) {
        std::string verify_error;
        ok = FileContentsEqual(full, text, &verify_error);
    }
    if (ok && recovery_path) *recovery_path = full;
    g_free(full);
    return ok;
}

static bool PreservePreviousSave(const char *full)
{
    if (!g_file_test(full, G_FILE_TEST_IS_REGULAR)) {
        return false;
    }

    gchar *old_contents = nullptr;
    gsize old_len = 0;
    GError *read_err = nullptr;
    if (!g_file_get_contents(full, &old_contents, &old_len, &read_err)) {
        fprintf(stderr, "[codes] warning: could not read previous save for "
                        "backup: %s\n",
                read_err ? read_err->message : "unknown error");
        if (read_err) g_error_free(read_err);
        return false;
    }

    std::string previous(old_contents, old_len);
    char *bak = g_strdup_printf("%s.bak", full);
    GError *write_err = nullptr;
    bool ok = g_file_set_contents(bak, previous.data(),
                                  (gssize)previous.size(), &write_err);
    if (ok) {
        std::string verify_error;
        ok = FileContentsEqual(bak, previous, &verify_error);
        if (!ok) {
            fprintf(stderr, "[codes] warning: %s\n", verify_error.c_str());
        }
    } else {
        fprintf(stderr, "[codes] warning: could not update backup %s: %s\n",
                bak, write_err ? write_err->message : "unknown error");
    }
    if (write_err) g_error_free(write_err);
    g_free(old_contents);
    g_free(bak);
    return ok;
}

static bool RestorePreviousSave(const char *full)
{
    char *bak = g_strdup_printf("%s.bak", full);
    gchar *contents = nullptr;
    gsize len = 0;
    bool ok = g_file_get_contents(bak, &contents, &len, nullptr);
    if (ok) {
        ok = g_file_set_contents(full, contents, (gssize)len, nullptr);
    }
    g_free(contents);
    g_free(bak);
    return ok;
}

bool CodesManager::SaveOne(Section &target, const Section &sec,
                           const char *folder, std::string *error,
                           std::string *path)
{
    if (error) error->clear();
    if (path) path->clear();
    if (m_stem.empty()) {
        const char *msg =
            "No running Xbox title is identified; code file was not saved.";
        if (error) *error = msg;
        xemu_queue_error_message(msg);
        return false;
    }

    std::string text = xcheat::RenderCheatText(
        sec.meta.game.empty() ? m_title : sec.meta.game,
        sec.meta.serial, sec.meta.titleid, sec.root, m_stem);

    char *configured_dir = CodesManager::DirFor(folder);
    std::string full;
    if (!target.source_path.empty()) {
        // Preserve the exact file discovered/loaded (including case and a
        // compatibility nested cheats/patches layout) instead of creating a
        // second canonical-case copy on Linux.
        full = target.source_path;
    } else {
        std::string save_dir = CodesPreferredDirectory(configured_dir, folder);
        char *fname = g_strdup_printf("%s.txt", m_stem.c_str());
        char *canonical = g_build_filename(save_dir.c_str(), fname, NULL);
        full = canonical ? canonical : "";
        g_free(canonical);
        g_free(fname);
    }
    g_free(configured_dir);

    if (path) *path = full;
    char *parent_c = g_path_get_dirname(full.c_str());
    std::string primary_dir = parent_c ? parent_c : "";
    g_free(parent_c);
    if (primary_dir.empty() ||
        g_mkdir_with_parents(primary_dir.c_str(), 0755) != 0) {
        int mkdir_errno = errno;
        std::string recovery;
        std::string msg = "Could not create code directory '" + primary_dir +
                          "': " + g_strerror(mkdir_errno) +
                          "\nPrimary path: " + full;
        if (WriteRecoveryCopy(m_stem, folder, text, &recovery)) {
            msg += "\nEmergency recovery copy saved to: " + recovery;
        } else {
            msg += "\nEmergency recovery copy could not be written either.";
        }
        if (error) *error = msg;
        fprintf(stderr, "[codes] %s\n", msg.c_str());
        xemu_queue_error_message(msg.c_str());
        return false;
    }

    const std::string tmp = full + ".tmp";

    auto fail = [&](const std::string &reason) {
        g_unlink(tmp.c_str());
        std::string recovery;
        std::string msg = reason + "\nPrimary path: " + full;
        if (WriteRecoveryCopy(m_stem, folder, text, &recovery)) {
            msg += "\nEmergency recovery copy saved to: " + recovery;
        } else {
            msg += "\nEmergency recovery copy could not be written either.";
        }
        if (error) *error = msg;
        fprintf(stderr, "[codes] %s\n", msg.c_str());
        xemu_queue_error_message(msg.c_str());
        return false;
    };

    // Clear a stale temp from an interrupted older save. Failure here is not
    // fatal if it did not exist; g_file_set_contents below reports a real
    // inability to replace it.
    g_unlink(tmp.c_str());

    GError *write_err = nullptr;
    if (!g_file_set_contents(tmp.c_str(), text.data(), (gssize)text.size(),
                             &write_err)) {
        std::string why = std::string("Could not write temporary code file: ") +
                          (write_err ? write_err->message : "unknown error");
        if (write_err) g_error_free(write_err);
        return fail(why);
    }
    if (write_err) g_error_free(write_err);

    std::string verify_error;
    if (!FileContentsEqual(tmp.c_str(), text, &verify_error)) {
        return fail(verify_error);
    }

    // Keep the previous successful file recoverable. A backup failure is
    // logged but does not block saving the new primary file; the primary save
    // remains the user's requested operation.
    bool have_current_backup = PreservePreviousSave(full.c_str());

    if (g_rename(tmp.c_str(), full.c_str()) != 0) {
        // POSIX rename replaces an existing destination atomically. Windows
        // CRT rename does not always do so, therefore fall back to moving the
        // old destination aside, installing the temp, and rolling back if the
        // second rename fails.
        int first_errno = errno;
        bool replaced = false;
        if (g_file_test(full.c_str(), G_FILE_TEST_EXISTS)) {
            std::string old = full + ".replace-old";
            g_unlink(old.c_str());
            if (g_rename(full.c_str(), old.c_str()) == 0) {
                if (g_rename(tmp.c_str(), full.c_str()) == 0) {
                    replaced = true;
                    g_unlink(old.c_str());
                } else {
                    int install_errno = errno;
                    if (g_rename(old.c_str(), full.c_str()) != 0) {
                        fprintf(stderr,
                                "[codes] CRITICAL: rollback rename failed for %s\n",
                                full.c_str());
                    }
                    return fail(std::string("Could not install new code file: ") +
                                g_strerror(install_errno));
                }
            }
        }
        if (!replaced) {
            return fail(std::string("Could not replace code file: ") +
                        g_strerror(first_errno));
        }
    }

    if (!FileContentsEqual(full.c_str(), text, &verify_error)) {
        if (have_current_backup && RestorePreviousSave(full.c_str())) {
            verify_error +=
                "\nThe previous code file was restored from its .bak copy.";
        } else {
            g_unlink(full.c_str());
            verify_error +=
                "\nThe unverified primary file was removed; use the reported "
                "recovery copy.";
        }
        return fail(verify_error);
    }

    target.source_path = full;
    return true;
}

bool CodesManager::Save(Section &sec, std::string *error, std::string *path)
{
    return SaveOne(sec, sec, &sec == &m_patches ? "patches" : "cheats",
                   error, path);
}

bool CodesManager::SaveReplacement(Section &target, const Section &contents,
                                   std::string *error, std::string *path)
{
    return SaveOne(target, contents,
                   &target == &m_patches ? "patches" : "cheats", error, path);
}

bool CodesManager::AddGeneratedAsmCheat(const char *name, const char *desc,
                                         const xcheat::Code *codes,
                                         size_t count, bool enabled)
{
    if (!codes || count == 0) {
        return false;
    }
    if (m_stem.empty() && !IdentifyGame()) {
        return false;
    }

    std::string base = name ? name : "";
    size_t first = 0;
    while (first < base.size() && std::isspace((unsigned char)base[first])) {
        ++first;
    }
    size_t last = base.size();
    while (last > first && std::isspace((unsigned char)base[last - 1])) {
        --last;
    }
    base = base.substr(first, last - first);
    if (base.empty()) {
        base = "Debugger patch";
    }
    if (!IsAsmName(base)) {
        base += " [ASM]";
    }

    // Match the convention already used by the user's database:
    //   [[ASM]\Patch name [ASM]]
    xcheat::Node *asm_group = nullptr;
    bool created_asm_group = false;
    for (auto &node : m_cheats.root) {
        if (node->is_group && node->name == "[ASM]") {
            asm_group = node.get();
            break;
        }
    }
    if (!asm_group) {
        auto group = std::make_unique<xcheat::Node>();
        group->is_group = true;
        group->name = "[ASM]";
        group->expanded = true;
        asm_group = group.get();
        m_cheats.root.push_back(std::move(group));
        created_asm_group = true;
    }

    // Do not silently create indistinguishable duplicates.
    std::string final_name = base;
    int suffix = 2;
    auto name_exists = [&](const std::string &candidate) {
        for (const auto &node : asm_group->children) {
            if (!node->is_group && node->name == candidate) {
                return true;
            }
        }
        return false;
    };
    while (name_exists(final_name)) {
        std::string stem = base;
        if (IsAsmName(stem)) {
            size_t end = stem.size();
            while (end && std::isspace((unsigned char)stem[end - 1])) --end;
            stem.erase(end - 5);
            while (!stem.empty() && std::isspace((unsigned char)stem.back()))
                stem.pop_back();
        }
        final_name = stem + " (" + std::to_string(suffix++) + ") [ASM]";
    }

    auto node = std::make_unique<xcheat::Node>();
    node->is_group = false;
    node->name = final_name;
    node->desc = desc ? desc : "";
    node->enabled = enabled;
    node->codes.assign(codes, codes + count);
    xcheat::Node *created = node.get();
    asm_group->children.push_back(std::move(node));

    std::string save_error;
    std::string save_path;
    if (!Save(m_cheats, &save_error, &save_path)) {
        // The debugger handoff is transactional. Do not leave a generated
        // block in memory and then tell the debugger it was saved when the
        // primary file was not committed. Save() has already preserved a
        // best-effort emergency recovery copy of the attempted tree.
        auto it = std::find_if(asm_group->children.begin(),
                               asm_group->children.end(),
                               [created](const auto &p) {
                                   return p.get() == created;
                               });
        if (it != asm_group->children.end()) {
            asm_group->children.erase(it);
        }
        if (created_asm_group && asm_group->children.empty()) {
            auto group_it = std::find_if(
                m_cheats.root.begin(), m_cheats.root.end(),
                [asm_group](const auto &p) { return p.get() == asm_group; });
            if (group_it != m_cheats.root.end()) {
                m_cheats.root.erase(group_it);
            }
        }
        return false;
    }
    RebuildLive();

    std::string saved_msg = "Cheat saved and verified: " + save_path;
    xemu_queue_notification(saved_msg.c_str());

    // If requested, take ownership of the live patch immediately rather than
    // waiting for the normal apply interval. This is what lets the debugger
    // restore its temporary patch first, then hand the same bytes to Cheats
    // without losing the original-byte journal.
    if (enabled && g_config.codes.enable) {
        CompiledBlock block;
        block.node = created;
        block.bid = (uint32_t)(uintptr_t)created;
        block.asm_patch = true;
        block.codes.reserve(created->codes.size());
        for (const auto &c : created->codes) {
            block.codes.push_back({c.cmd, c.val});
        }
        ApplyBlockNow(block);
    }
    return true;
}

void CodesManager::PrepareNodeMutation(xcheat::Node &node)
{
    if (node.is_group) {
        for (auto &child : node.children) {
            PrepareNodeMutation(*child);
        }
        return;
    }

    uint32_t bid = (uint32_t)(uintptr_t)&node;
    if (IsAsmName(node.name)) {
        // Restore before changing/removing the node. The journal is keyed by
        // node identity, so waiting until after erase would strand original
        // executable bytes under a dead id.
        m_mem.RestoreAsm(bid);
    }
    m_engine.ClearSwitches(bid, false);
}

bool CodesManager::FinishTreeMutation(Section &sec, xcheat::Node *changed,
                                      std::string *error, std::string *path)
{
    bool saved = Save(sec, error, path);
    if (!saved) {
        /* PrepareNodeMutation() may have restored old [ASM] bytes before an
         * edit/delete. The last successfully compiled m_live still contains
         * the disk-backed blocks, so put those executable patches back now
         * and, crucially, do not compile the unsaved candidate tree. */
        if (g_config.codes.enable) {
            for (const auto &block : m_live) {
                if (block.asm_patch) {
                    ApplyBlockNow(block);
                }
            }
        }
        return false;
    }

    RebuildLive();

    // Match the existing checkbox behavior: an enabled [ASM] edit should take
    // ownership of its new bytes immediately instead of waiting for the next
    // configured codes interval. Ordinary data cheats continue through Tick().
    // Do not immediately apply a newly edited executable patch when its primary
    // persistence failed; the error/recovery path must be resolved first.
    if (changed && !changed->is_group && changed->enabled &&
        IsAsmName(changed->name) && g_config.codes.enable) {
        for (const auto &block : m_live) {
            if (block.node == changed) {
                ApplyBlockNow(block);
                break;
            }
        }
    }
    return true;
}

bool FeatureCodesAddGeneratedAsmCheat(const char *name, const char *desc,
                                      const uint32_t *cmds,
                                      const uint32_t *vals,
                                      size_t count, bool enabled)
{
    if (!cmds || !vals || count == 0) {
        return false;
    }
    std::vector<xcheat::Code> codes;
    codes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        // The bridge accepts the existing raw format verbatim. In particular,
        // this does NOT assign any meaning to reserved Type F. Debugger-
        // generated patches currently use only 8/9/A.
        codes.push_back({cmds[i], vals[i]});
    }
    return g_codes.AddGeneratedAsmCheat(name, desc, codes.data(), codes.size(),
                                        enabled);
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

void CodesManager::Tick()
{
    /*
     * File discovery is UI state, not cheat-execution state. Keep identifying
     * the running title even when the master switch is off so Cheats/Patches
     * never goes blank merely because code execution is disabled.
     *
     * The previous `|| m_stem.empty()` also retried the synchronized XBE read
     * every UI frame while no title was available. A fixed 500 ms cadence is
     * both sufficient for title changes and much cheaper at the dashboard.
     */
    uint32_t now = SDL_GetTicks();
    if (m_last_identify_ms == 0 || now - m_last_identify_ms >= 500) {
        m_last_identify_ms = now;
        IdentifyGame();
    }

    if (!g_config.codes.enable) {
        // [ASM] differs deliberately from an ordinary freeze: when the whole
        // codes feature is switched off, put executable bytes back exactly as
        // an individual [ASM] checkbox would. Re-enabling journals/apply them
        // again from the current guest bytes.
        if (m_runtime_enabled_last) {
            m_mem.RestoreAllAsm();
            m_runtime_enabled_last = false;
        }
        return;
    }
    m_runtime_enabled_last = true;

    if (m_stem.empty()) return;

    /*
     * Apply-rate throttle. Re-applying every cheat at 60 Hz is wasted work:
     * a freeze only needs to beat the game's own writes, not the frame rate.
     * Configurable because a value the game rewrites every frame needs a
     * tight interval, while a one-shot patch is fine at 1000 ms.
     */
    int interval = g_config.codes.interval_ms;
    if (interval < 0) interval = 0;
    if (interval > 1000) interval = 1000;

    /*
     * interval == 0 is the explicit Instantaneous mode.  It removes the
     * millisecond throttle completely, so enabled cheats/patches are applied
     * on every CodesManager::Tick() call (the fastest safe rate supported by
     * the existing UI-thread/BQL integration).
     */
    if (interval != 0) {
        if (now - m_last_apply_ms < (uint32_t)interval) {
            return;
        }
        m_last_apply_ms = now;
    }

    /* Enabled blocks are compiled when the tree changes, never in the apply
     * loop. Instantaneous mode can call Tick hundreds/thousands of times per
     * second, so recursive tree walks and CodeList allocation/copying do not
     * belong here. */
    if (m_live.empty()) return;

    /* The caches assume paging is stable for one apply pass. Invalidate in
     * O(1); the engine's Memory attachment itself never changes. */
    xemu_guestmem_invalidate_cache();
    if (!m_engine_attached) {
        m_engine.Attach(&m_mem);
        m_engine_attached = true;
    } else {
        m_engine.InvalidatePageMap();
    }

    for (const auto &block : m_live) {
        if (block.asm_patch) {
            m_mem.BeginAsmBlock(block.bid);
        }
        m_engine.ExecuteBlock(block.codes, block.bid);
        if (block.asm_patch) {
            m_mem.EndAsmBlock();
        }
    }
}


// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

// Groups are TreeNodeEx and cheats are Checkbox, with feature-owned context
// menus for per-entry editing. The full .txt editor remains available as the
// lossless/raw route; these controls are the fast path for normal work.
void CodesManager::DrawTree(xcheat::NodeList &nodes, int depth, Section *sec)
{
    for (auto &n : nodes) {
        ImGui::PushID(n.get());
        if (n->is_group) {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (n->expanded) flags |= ImGuiTreeNodeFlags_DefaultOpen;
            bool open = ImGui::TreeNodeEx(n->name.c_str(), flags);
            if (sec) DrawNodeContextMenu(*sec, nodes, *n);
            if (open) {
                DrawTree(n->children, depth + 1, sec);
                ImGui::TreePop();
            }
        } else {
            bool old_on = n->enabled;
            bool on = old_on;
            if (ImGui::Checkbox(n->name.c_str(), &on)) {
                n->enabled = on;

                // A checkbox is a disk-backed state change. Do not alter the
                // running cheat state unless the new enabled flag has actually
                // reached and passed verification at the primary file path.
                std::string save_error;
                std::string save_path;
                bool saved = !sec || Save(*sec, &save_error, &save_path);
                if (!saved) {
                    n->enabled = old_on;
                    RebuildLive();
                } else {
                    if (sec) {
                        std::string msg = "Cheat state saved and verified: " +
                                          save_path;
                        xemu_queue_notification(msg.c_str());
                    }
                    if (!on) {
                        uint32_t bid = (uint32_t)(uintptr_t)n.get();
                        if (IsAsmName(n->name)) {
                            // Native port of the external trainer's [ASM] journal:
                            // disabling an assembly patch restores the original
                            // executable bytes immediately.
                            auto r = m_mem.RestoreAsm(bid);
                            if (r.restored_ranges || r.failed_ranges) {
                                char msg[192];
                                snprintf(msg, sizeof(msg),
                                         "[ASM] restored %zu original range(s)%s",
                                         r.restored_ranges,
                                         r.failed_ranges ? "; some ranges failed" : "");
                                if (r.failed_ranges) xemu_queue_error_message(msg);
                                else xemu_queue_notification(msg);
                            }
                        }
                        // Clear this cheat's switch and increment state so a
                        // re-enable re-arms a type 3 rather than being a no-op,
                        // and a type E switch does not come back on.
                        m_engine.ClearSwitches(bid, false);
                    }
                    RebuildLive();

                    // Assembly patches are expected to be interactive: enabling
                    // one should take effect now, not after the next configured
                    // cheat interval. Applying through the normal compiled block
                    // path also captures its original bytes before the first
                    // write, so disabling it can restore them exactly.
                    if (on && IsAsmName(n->name) && g_config.codes.enable) {
                        for (const auto &block : m_live) {
                            if (block.node == n.get()) {
                                ApplyBlockNow(block);
                                break;
                            }
                        }
                    }
                }
            }
            if (sec) DrawNodeContextMenu(*sec, nodes, *n);
            if (!n->desc.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", n->desc.c_str());
            }
            if (!n->author.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", n->author.c_str());
            }
        }
        ImGui::PopID();
    }
}

void CodesManager::DrawSection(Section &sec, const char *empty_msg)
{
    if (m_stem.empty()) {
        ImGui::TextDisabled("No game running.");
        return;
    }
    if (sec.root.empty()) {
        ImGui::TextDisabled("%s", empty_msg);
        return;
    }
    DrawTree(sec.root, 0, &sec);
}

void MainMenuCodesView::Draw()
{
    /*
     * Legacy Settings > Codes view intentionally retired.  The feature-owned
     * Misc > Cheats/Patches window now contains the master enable/apply timing,
     * file locations, groups, and individual cheat/patch editors.
     *
     * The sidebar entry itself is suppressed by the feature-owned settings
     * integration shim without modifying native Xemu UI sources.
     */
}

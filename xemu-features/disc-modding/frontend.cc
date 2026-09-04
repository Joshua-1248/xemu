// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - Xbox disc browser / per-title file override UI
#include "xemu-features/disc-modding/frontend.hh"
#include "xemu-features/disc-modding/core.hh"

#include "ui/xui/common.hh"
#include "ui/xui/misc.hh"
#include "ui/xemu-notifications.h"
#include "xemu-features/shared/detachable-windows.hh"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace xemu_disc_modding;

namespace {

constexpr const char *kDetachId = "disc-files-mods.window";
bool g_window_open = false;
size_t g_selected = kNoIndex;
CollisionPolicy g_collision_policy = CollisionPolicy::Rename;
std::string g_mod_base_edit;
bool g_mod_base_initialized = false;
std::string g_ui_message;

static const char *CollisionPolicyName(CollisionPolicy p)
{
    switch (p) {
    case CollisionPolicy::Skip: return "Skip existing";
    case CollisionPolicy::Overwrite: return "Overwrite existing";
    case CollisionPolicy::Rename: return "Auto-rename";
    }
    return "Auto-rename";
}

static std::string FormatBytes(uint64_t n)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = double(n);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    if (unit == 0) {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    } else {
        snprintf(buf, sizeof(buf), "%.2f %s", value, units[unit]);
    }
    return buf;
}

static void NotifyResult(const std::string &prefix, const std::string &error)
{
    if (error.empty()) {
        g_ui_message = prefix;
        xemu_queue_notification(prefix.c_str());
    } else {
        g_ui_message = error;
        xemu_queue_notification(error.c_str());
    }
}

static void StartExtractDialog(const std::shared_ptr<const Snapshot> &snapshot,
                               size_t index, bool entire_disc)
{
    if (!snapshot) return;
    ShowOpenFolderDialog(nullptr, [snapshot, index, entire_disc](const char *path) {
        if (!path || !path[0]) return;
        std::string error;
        if (!StartExtraction(snapshot, index, entire_disc, fs::u8path(path),
                             g_collision_policy, false, &error)) {
            NotifyResult("", "Disc extraction: " + error);
        } else {
            NotifyResult("Disc extraction started", "");
        }
    });
}

static void CopyOriginalAsOverride(const std::shared_ptr<const Snapshot> &snapshot,
                                   size_t index)
{
    if (!snapshot || index >= snapshot->entries.size() ||
        snapshot->entries[index].directory) return;
    std::string error;
    if (!EnsureOverrideParent(*snapshot, index, &error)) {
        NotifyResult("", "Disc Mods: " + error);
        return;
    }
    if (!StartExtraction(snapshot, index, false, fs::path(),
                         CollisionPolicy::Overwrite, true, &error)) {
        NotifyResult("", "Disc Mods: " + error);
    } else {
        NotifyResult("Copying original file into the per-title mod tree", "");
    }
}

static void DrawEntryContextMenu(const std::shared_ptr<const Snapshot> &snapshot,
                                 size_t index)
{
    const Entry &e = snapshot->entries[index];
    if (ImGui::MenuItem(e.directory ? "Extract Directory..." : "Extract File...")) {
        StartExtractDialog(snapshot, index, false);
    }
    if (!snapshot->effective_mod_root.empty()) {
        if (ImGui::MenuItem("Create Override Folder")) {
            std::string error;
            if (EnsureOverrideParent(*snapshot, index, &error)) {
                NotifyResult("Override folder created", "");
            } else {
                NotifyResult("", "Disc Mods: " + error);
            }
        }
        if (!e.directory && ImGui::MenuItem("Copy Original as Override")) {
            CopyOriginalAsOverride(snapshot, index);
        }
        fs::path override_path = OverridePathForEntry(*snapshot, index);
        if (!override_path.empty() && ImGui::MenuItem("Copy Override Path")) {
            ImGui::SetClipboardText(override_path.u8string().c_str());
        }
    }
    if (ImGui::MenuItem("Copy Internal Path")) {
        ImGui::SetClipboardText(e.path.c_str());
    }
}

static void DrawTreeEntry(const std::shared_ptr<const Snapshot> &snapshot,
                          size_t index)
{
    if (index >= snapshot->entries.size()) return;
    const Entry &e = snapshot->entries[index];
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID((int)index);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns |
                               ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (!e.directory || e.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (g_selected == index) flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx("##entry", flags, "%s%s",
                                        e.directory ? "[DIR] " : "", e.name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_selected = index;
    if (ImGui::BeginPopupContextItem("##entry_context")) {
        g_selected = index;
        DrawEntryContextMenu(snapshot, index);
        ImGui::EndPopup();
    }

    ImGui::TableSetColumnIndex(1);
    if (!e.directory) ImGui::TextUnformatted(FormatBytes(e.size).c_str());
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(e.directory ? "Directory" : "File");
    ImGui::TableSetColumnIndex(3);
    ImGui::Text("0x%08X", e.start_sector);
    ImGui::TableSetColumnIndex(4);
    if (e.override_active) {
        ImGui::Text("Active (%s)", FormatBytes(e.override_size).c_str());
    } else if (e.override_present) {
        ImGui::TextUnformatted(snapshot->overlay_enabled ? "Present / rejected" : "Present / disabled");
    } else {
        ImGui::TextDisabled("-");
    }

    if (open) {
        if (e.directory) {
            for (size_t child : e.children) DrawTreeEntry(snapshot, child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

static void DrawExtractionProgress()
{
    ExtractProgress p = GetExtractionProgress();
    if (!p.active && !p.completed) return;

    ImGui::SeparatorText("Extraction");
    float fraction = p.bytes_total ? float(double(p.bytes_done) / double(p.bytes_total))
                                   : (p.completed ? 1.0f : 0.0f);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    char overlay[128];
    snprintf(overlay, sizeof(overlay), "%zu / %zu files - %s / %s",
             p.files_done, p.files_total, FormatBytes(p.bytes_done).c_str(),
             FormatBytes(p.bytes_total).c_str());
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);
    if (!p.current_path.empty()) ImGui::TextWrapped("%s", p.current_path.c_str());
    if (!p.destination.empty()) ImGui::TextWrapped("Destination: %s", p.destination.c_str());
    if (p.active) {
        if (p.cancel_requested) {
            ImGui::TextUnformatted("Cancelling...");
        } else if (ImGui::Button("Cancel Extraction")) {
            CancelExtraction();
        }
    } else if (p.cancelled) {
        ImGui::TextUnformatted("Extraction cancelled.");
    } else if (p.success) {
        ImGui::TextUnformatted("Extraction complete.");
    } else if (!p.error.empty()) {
        ImGui::TextWrapped("Extraction error: %s", p.error.c_str());
    }
}

static void DrawFilesTab(const std::shared_ptr<const Snapshot> &snapshot)
{
    if (!snapshot || !snapshot->valid) {
        ImGui::TextWrapped("%s", snapshot ? snapshot->parse_error.c_str() : "No disc snapshot available.");
        return;
    }

    ImGui::Text("Entries: %zu", snapshot->entries.size() > 0 ? snapshot->entries.size() - 1 : 0);
    ImGui::SameLine();
    if (ImGui::Button("Extract Entire Disc...")) {
        StartExtractDialog(snapshot, 0, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload Disc / Overrides")) Reload();

    ImGui::Separator();
    const ImGuiTableFlags table_flags = ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_Resizable |
                                        ImGuiTableFlags_ScrollY |
                                        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##xdvdfs_tree", 5, table_flags, ImVec2(0, -190.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Sector / LBA", ImGuiTableColumnFlags_WidthFixed, 105.0f);
        ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (size_t child : snapshot->entries[0].children) DrawTreeEntry(snapshot, child);
        ImGui::EndTable();
    }

    if (g_selected < snapshot->entries.size()) {
        const Entry &e = snapshot->entries[g_selected];
        ImGui::SeparatorText("Selected");
        ImGui::TextWrapped("Internal path: %s", e.path.c_str());
        ImGui::Text("Start sector: %u (0x%08X)", e.start_sector, e.start_sector);
        ImGui::Text("Original size: %s", FormatBytes(e.size).c_str());
        fs::path override_path = OverridePathForEntry(*snapshot, g_selected);
        if (!override_path.empty()) ImGui::TextWrapped("Override path: %s", override_path.u8string().c_str());
        if (!e.directory) {
            if (ImGui::Button("Extract Selected...")) StartExtractDialog(snapshot, g_selected, false);
            ImGui::SameLine();
            if (ImGui::Button("Copy Original as Override")) CopyOriginalAsOverride(snapshot, g_selected);
        } else {
            if (ImGui::Button("Extract Selected Directory...")) StartExtractDialog(snapshot, g_selected, false);
        }
    }
    DrawExtractionProgress();
}

static void DrawModsTab(const std::shared_ptr<const Snapshot> &snapshot)
{
    bool enabled = GetOverlayEnabled();
    if (ImGui::Checkbox("Enable per-title disc-file overrides", &enabled)) {
        SetOverlayEnabled(enabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Replacement files mirror the Xbox disc path. The source XISO is never modified.");
    }

    if (!g_mod_base_initialized) {
        g_mod_base_edit = GetConfiguredModBase();
        g_mod_base_initialized = true;
    }
    ImGui::TextUnformatted("Mod library root");
    ImGui::SetNextItemWidth(-190.0f);
    ImGui::InputTextWithHint("##mod_base", GetDefaultModBase().c_str(), &g_mod_base_edit);
    ImGui::SameLine();
    if (ImGui::Button("Apply", ImVec2(55, 0))) SetConfiguredModBase(g_mod_base_edit);
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(80, 0))) {
        const std::string start = g_mod_base_edit.empty() ? GetDefaultModBase() : g_mod_base_edit;
        ShowOpenFolderDialog(start.c_str(), [](const char *path) {
            if (!path || !path[0]) return;
            g_mod_base_edit = path;
            SetConfiguredModBase(g_mod_base_edit);
        });
    }
    if (!g_mod_base_edit.empty() && ImGui::Button("Use Default Mod Root")) {
        g_mod_base_edit.clear();
        SetConfiguredModBase("");
    }
    ImGui::TextWrapped("Default: %s", GetDefaultModBase().c_str());

    if (snapshot && snapshot->valid) {
        ImGui::SeparatorText("Current title");
        if (snapshot->title_id) ImGui::Text("Title ID: %08X", snapshot->title_id);
        if (!snapshot->title_name.empty()) ImGui::TextWrapped("Title: %s", snapshot->title_name.c_str());
        ImGui::TextWrapped("Disc image: %s", snapshot->source_path.c_str());
        if (!snapshot->effective_mod_root.empty()) {
            ImGui::TextWrapped("Per-title override root: %s", snapshot->effective_mod_root.c_str());
            if (ImGui::Button("Create Per-Title Mod Root")) {
                std::error_code ec;
                fs::create_directories(fs::u8path(snapshot->effective_mod_root), ec);
                if (ec) NotifyResult("", "Disc Mods: " + ec.message());
                else NotifyResult("Per-title mod root created", "");
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy Mod Root Path")) ImGui::SetClipboardText(snapshot->effective_mod_root.c_str());
        }
        ImGui::Text("Active overrides: %zu", snapshot->active_overrides);
        ImGui::Text("Unmatched override files: %zu", snapshot->unmatched_override_files);
        ImGui::Text("Rejected override files: %zu", snapshot->rejected_override_files);
        if (snapshot->ambiguous_disc_paths || snapshot->ambiguous_override_paths) {
            ImGui::Text("Ambiguous paths: disc=%zu, overrides=%zu",
                        snapshot->ambiguous_disc_paths, snapshot->ambiguous_override_paths);
        }
        if (!snapshot->warning.empty()) ImGui::TextWrapped("Warning: %s", snapshot->warning.c_str());
        for (const std::string &m : snapshot->overlay_messages) {
            ImGui::BulletText("%s", m.c_str());
        }
    } else if (snapshot) {
        ImGui::TextWrapped("%s", snapshot->parse_error.c_str());
    }

    ImGui::SeparatorText("Extraction collision policy");
    int policy = int(g_collision_policy);
    const char *items[] = {"Skip existing", "Overwrite existing", "Auto-rename"};
    if (ImGui::Combo("Existing files", &policy, items, 3)) {
        g_collision_policy = CollisionPolicy(policy);
    }
    ImGui::TextWrapped("Current policy: %s", CollisionPolicyName(g_collision_policy));

    ImGui::SeparatorText("Safety / reload behavior");
    ImGui::TextWrapped(
        "Overrides are resolved as a case-insensitive Xbox path mirror. Dot/dot-dot traversal, symlink escapes, ambiguous case collisions, non-regular files, and replacements larger than XDVDFS can represent are rejected. Changing any replacement file requires Reload Disc / Overrides; restarting the title after such a change is safest because games may cache filesystem metadata.");
    ImGui::TextWrapped(
        "Directory browsing and extraction read the original XISO only. Replacement data is exposed to the guest through feature-owned virtual sectors; the XISO itself is never repacked or written.");

    const std::string runtime_error = GetRuntimeOverlayError();
    if (!runtime_error.empty()) ImGui::TextWrapped("Runtime override error: %s", runtime_error.c_str());
    if (!g_ui_message.empty()) ImGui::TextWrapped("Status: %s", g_ui_message.c_str());
}

} // namespace

void FeatureDiscModdingDrawMiscMenuItem()
{
    ImGui::MenuItem("Disc Files && Mods", nullptr, &g_window_open);
}

void FeatureDiscModdingDrawWindow()
{
    xemu_feature_detach::Register(kDetachId, "Disc Files & Mods", &g_window_open,
                                  []() { FeatureDiscModdingDrawWindow(); });
    xemu_feature_detach::Pump();
    if (!g_window_open || !xemu_feature_detach::ShouldDraw(kDetachId)) return;

    if (xemu_feature_detach::IsDetachedPass(kDetachId)) {
        xemu_feature_detach::PrepareWindow(kDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(900.0f, 680.0f), ImGuiCond_FirstUseEver);
    }
    const ImGuiWindowFlags flags = xemu_feature_detach::WindowFlags(kDetachId, 0);
    if (!ImGui::Begin("Disc Files & Mods", &g_window_open, flags)) {
        ImGui::End();
        return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kDetachId);

    auto snapshot = GetSnapshot();
    if (snapshot && g_selected >= snapshot->entries.size()) g_selected = kNoIndex;

    if (ImGui::BeginTabBar("##disc_files_tabs")) {
        if (ImGui::BeginTabItem("Disc Files")) {
            DrawFilesTab(snapshot);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Mods / Settings")) {
            DrawModsTab(snapshot);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

bool FeatureDiscModdingWindowOpen() { return g_window_open; }

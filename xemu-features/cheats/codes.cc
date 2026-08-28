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
#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"
#include "xemu-xbe.h"
#include "xemu-features/shared/guest-memory.h"

#include <cstdio>
#include <cinttypes>

CodesManager g_codes;

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
                sec->root = std::move(parsed_root);
                sec->meta = std::move(parsed_meta);

                // Save atomically through the existing writer, then reload the
                // tree. LoadForCurrentGame() also clears interpreter switch
                // and one-shot state, which is required when node addresses
                // may have changed.
                g_codes.Save(*sec);
                g_codes.LoadForCurrentGame();

                ImGui::CloseCurrentPopup();
                g_codes_editor = CodesEditorState{};
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

} // anonymous namespace

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
    // Failure is deliberately silent: a cheat aimed at an address the running
    // title has not mapped is a bad cheat, not an emulator fault, and warning
    // once per frame at 60 Hz would bury the log.
    xemu_phys_write(off, buf, len);
}

uint32_t GuestMemory::RamSize() const
{
    return (uint32_t)xemu_guest_ram_size();
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

void CodesManager::LoadOne(Section &sec, const char *folder)
{
    sec.root.clear();
    sec.meta = xcheat::Meta();
    if (m_stem.empty()) return;

    // <basedir>/cheats/<stem>.txt  and  <basedir>/patches/<stem>.txt
    //
    // xemu_settings_get_base_path() is the same root texture-io.c builds its
    // per-title folders from, so codes and texture packs live side by side in
    // one place the user already knows about.
    char *dir = CodesManager::DirFor(folder);
    char *fname = g_strdup_printf("%s.txt", m_stem.c_str());
    char *full = g_build_filename(dir, fname, NULL);
    g_free(fname);
    g_free(dir);

    gchar *contents = NULL;
    gsize len = 0;
    bool ok = g_file_get_contents(full, &contents, &len, NULL);
    g_free(full);
    if (!ok) {
        return;                          // no file for this game: not an error
    }
    xcheat::ParseCheatText(std::string(contents, len), &sec.root, &sec.meta);
    g_free(contents);

    // Warnings are printed, not swallowed. A hand-edited file with a typo
    // should say so somewhere findable.
    for (const auto &w : sec.meta.warnings) {
        fprintf(stderr, "[codes] %s: %s\n", m_stem.c_str(), w.c_str());
    }
}

void CodesManager::LoadForCurrentGame()
{
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

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------
// Written the moment a checkbox changes, NOT at exit.
//
// xemu.c installs atexit(xemu_settings_save), which does not run on a crash,
// a SIGKILL, or a force-close - exactly the cases where losing state is most
// annoying. Enabled flags live in the cheat files rather than the config, so
// they are saved here on the same principle: immediately, and atomically.
//
// Temp file then rename, so an interrupted write cannot truncate a file the
// user may have spent time on. Same approach as cheatfiles.write_cheat_file.

void CodesManager::SaveOne(Section &sec, const char *folder)
{
    if (m_stem.empty()) return;

    char *dir = CodesManager::DirFor(folder);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        fprintf(stderr, "[codes] could not create %s\n", dir);
        g_free(dir);
        return;
    }

    char *fname = g_strdup_printf("%s.txt", m_stem.c_str());
    char *full = g_build_filename(dir, fname, NULL);
    char *tmp = g_strdup_printf("%s.tmp", full);
    g_free(fname);
    g_free(dir);

    std::string text = xcheat::RenderCheatText(
        sec.meta.game.empty() ? m_title : sec.meta.game,
        sec.meta.serial, sec.meta.titleid, sec.root, m_stem);

    GError *err = NULL;
    if (!g_file_set_contents(tmp, text.c_str(), (gssize)text.size(), &err)) {
        fprintf(stderr, "[codes] write failed: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
    } else if (g_rename(tmp, full) != 0) {
        fprintf(stderr, "[codes] rename failed for %s\n", full);
        g_unlink(tmp);
    }

    g_free(tmp);
    g_free(full);
}

void CodesManager::Save(Section &sec)
{
    SaveOne(sec, &sec == &m_patches ? "patches" : "cheats");
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

void CodesManager::Tick()
{
    if (!g_config.codes.enable) return;

    /*
     * Game identification synchronizes CPU state and reads guest XBE fields.
     * Even the lightweight title-ID helper does not belong on every UI tick.
     * A game cannot change without a disc swap or reset, so checking a few
     * times a second preserves responsiveness without continual guest reads.
     */
    uint32_t now = SDL_GetTicks();
    if (now - m_last_identify_ms >= 500 || m_stem.empty()) {
        m_last_identify_ms = now;
        IdentifyGame();
    }
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
        m_engine.ExecuteBlock(block.codes, block.bid);
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

// Groups are TreeNodeEx, cheats are Checkbox. ImGui has no Treeview, so this
// is the closest equivalent; drag-to-reparent and in-place editing are
// deliberately NOT here - use the external trainer or a text editor for those.
void CodesManager::DrawTree(xcheat::NodeList &nodes, int depth, Section *sec)
{
    for (auto &n : nodes) {
        ImGui::PushID(n.get());
        if (n->is_group) {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (n->expanded) flags |= ImGuiTreeNodeFlags_DefaultOpen;
            if (ImGui::TreeNodeEx(n->name.c_str(), flags)) {
                DrawTree(n->children, depth + 1, sec);
                ImGui::TreePop();
            }
        } else {
            bool on = n->enabled;
            if (ImGui::Checkbox(n->name.c_str(), &on)) {
                n->enabled = on;
                // Persist right now rather than at exit -- see SaveOne().
                if (sec) Save(*sec);
                if (!on) {
                    // Clear this cheat's switch and increment state so a
                    // re-enable re-arms a type 3 rather than being a no-op,
                    // and a type E switch does not come back on.
                    m_engine.ClearSwitches((uint32_t)(uintptr_t)n.get(), false);
                }
                RebuildLive();
            }
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
    SectionTitle("General");

    Toggle("Enable codes", &g_config.codes.enable,
           "Apply enabled cheats and patches while a game is running");

    if (!g_codes.Stem().empty()) {
        ImGui::TextDisabled("%s  (%s)", g_codes.Title().c_str(),
                            g_codes.Stem().c_str());
    }

    int iv = g_config.codes.interval_ms;
    if (iv < 0) iv = 0;
    if (iv > 1000) iv = 1000;

    /* Preserve the user's last timed interval when Instantaneous is toggled. */
    static int last_timed_interval = 16;
    if (iv > 0) {
        last_timed_interval = iv;
    }

    bool instantaneous = (iv == 0);
    if (ImGui::Checkbox("Instantaneous", &instantaneous)) {
        if (instantaneous) {
            g_config.codes.interval_ms = 0;
            iv = 0;
        } else {
            iv = last_timed_interval;
            if (iv < 1) iv = 1;
            if (iv > 1000) iv = 1000;
            g_config.codes.interval_ms = iv;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Remove the apply-rate throttle and run enabled "
                          "cheats/patches on every codes tick.");
    }

    if (instantaneous) {
        ImGui::SameLine();
        ImGui::TextDisabled("No interval throttle (maximum apply rate)");
    } else {
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderInt("Apply interval (ms)", &iv, 1, 1000)) {
            if (iv < 1) iv = 1;
            if (iv > 1000) iv = 1000;
            last_timed_interval = iv;
            g_config.codes.interval_ms = iv;
        }
        ImGui::TextDisabled("How often enabled codes are re-applied. Lower is "
                            "more responsive, higher is faster.");
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().WindowPadding.y));
    ImGui::TextWrapped(
        "Leave a directory blank to use the default location inside the xemu "
        "data directory. Files are named <SERIAL>_<TITLEID>.txt.");
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().WindowPadding.y));

    SectionTitle("Cheats");
    FilePicker("Cheats directory", g_config.codes.cheats_dir, nullptr, 0, true,
               [](const char *path) {
                   xemu_settings_set_string(&g_config.codes.cheats_dir, path);
                   g_codes.LoadForCurrentGame();
               });

    if (g_codes.Stem().empty()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Edit cheats...")) {
        OpenCodesEditor(g_codes.Cheats(), "Cheats");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload cheats")) {
        g_codes.LoadForCurrentGame();
    }
    if (g_codes.Stem().empty()) {
        ImGui::EndDisabled();
    }

    g_codes.DrawSection(g_codes.Cheats(), "No cheats for this game.");

    SectionTitle("Patches");
    FilePicker("Patches directory", g_config.codes.patches_dir, nullptr, 0,
               true, [](const char *path) {
                   xemu_settings_set_string(&g_config.codes.patches_dir, path);
                   g_codes.LoadForCurrentGame();
               });

    if (g_codes.Stem().empty()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Edit patches...")) {
        OpenCodesEditor(g_codes.Patches(), "Patches");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload patches")) {
        g_codes.LoadForCurrentGame();
    }
    if (g_codes.Stem().empty()) {
        ImGui::EndDisabled();
    }

    g_codes.DrawSection(g_codes.Patches(), "No patches for this game.");
    DrawCodesEditor();
}

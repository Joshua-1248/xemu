//
// xemu custom fork - isolated texture pack frontend
//
// Copyright (C) 2026 Joshua-1248
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "frontend.hh"

#include "ui/xui/common.hh"
#include "ui/xui/widgets.hh"
#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"

extern "C" {
#include "xemu-features/texture-packs/texture-packs.h"
}

#include <stdio.h>

/* Owned entirely by this feature. The generic HUD renderer only calls the
 * narrow commit hook after drawing. */
static bool g_reload_pending;

static void HotkeyBinder(const char *label, int *key_value)
{
    static const char *capturing_label = nullptr;
    bool capturing = (capturing_label == label);

    const char *key_name = "(unset)";
    if (!capturing && *key_value > 0) {
        const char *n = ImGui::GetKeyName((ImGuiKey)*key_value);
        if (n && n[0]) {
            key_name = n;
        }
    }

    ImGui::PushID(label);
    ImGui::Text("%s", label);
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);

    char btn[128];
    snprintf(btn, sizeof(btn), "%s##bind",
             capturing ? "Press a key..." : key_name);

    if (ImGui::Button(btn, ImVec2(ImGui::GetWindowWidth() * 0.35f, 0))) {
        capturing_label = capturing ? nullptr : label;
        capturing = !capturing;
    }

    if (capturing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            capturing_label = nullptr;
        } else {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                if (ImGui::IsKeyPressed((ImGuiKey)k)) {
                    *key_value = k;
                    capturing_label = nullptr;
                    break;
                }
            }
        }
    }
    ImGui::PopID();
}

void FeatureTexturePacksDrawSettings()
{
    SectionTitle("Texture Packs");
    if (Toggle("Dump textures", &g_config.general.texture_dump_enabled,
               "Write decoded textures to the dump directory as they are used")) {
        if (g_config.general.texture_dump_enabled) {
            xemu_texture_packs_rebuild_dump_index();
        }
    }
    if (Toggle("Use texture replacements",
               &g_config.general.texture_replace_enabled,
               "Load replacement textures from the replacements directory")) {
        xemu_texture_packs_rebuild_replacement_index();
    }
    if (Toggle("Skip already-replaced textures when dumping",
               &g_config.general.texture_dump_skip_replaced,
               "Do not dump textures that already have a replacement")) {
        xemu_texture_packs_rebuild_replacement_index();
    }
    Toggle("Dump mipmaps", &g_config.general.texture_dump_mipmaps,
           "Also dump smaller mip levels, not just the full-size texture");

    FilePicker("Texture dump directory", g_config.general.texture_dump_dir,
               nullptr, 0, true, [](const char *path) {
                   xemu_settings_set_string(&g_config.general.texture_dump_dir,
                                            path);
                   xemu_texture_packs_refresh_paths();
               });
    FilePicker("Texture replacement directory",
               g_config.general.texture_replace_dir, nullptr, 0, true,
               [](const char *path) {
                   xemu_settings_set_string(
                       &g_config.general.texture_replace_dir, path);
                   xemu_texture_packs_refresh_paths();
               });

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().WindowPadding.y));
    ImGui::TextWrapped(
        "Leave directories blank to use the default location inside the xemu "
        "data directory. A per-title subfolder is always added.");
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().WindowPadding.y));

    HotkeyBinder("Toggle dumping", &g_config.general.texture_dump_toggle_key);
    HotkeyBinder("Toggle replacements",
                 &g_config.general.texture_replace_toggle_key);
    HotkeyBinder("Reload replacements",
                 &g_config.general.texture_replace_reload_key);
}

void FeatureTexturePacksFrameSync()
{
    xemu_texture_packs_refresh_paths();
}

void FeatureTexturePacksProcessHotkeys()
{
    if (ImGui::IsKeyPressed(
            (enum ImGuiKey)g_config.general.texture_replace_toggle_key)) {
        g_config.general.texture_replace_enabled =
            !g_config.general.texture_replace_enabled;
        g_reload_pending = true;
        xemu_queue_notification(
            g_config.general.texture_replace_enabled ?
                "Texture replacements: ON" :
                "Texture replacements: OFF");
    }

    if (ImGui::IsKeyPressed(
            (enum ImGuiKey)g_config.general.texture_replace_reload_key)) {
        g_reload_pending = true;
        xemu_queue_notification("Reloading texture replacements");
    }

    if (ImGui::IsKeyPressed(
            (enum ImGuiKey)g_config.general.texture_dump_toggle_key)) {
        g_config.general.texture_dump_enabled =
            !g_config.general.texture_dump_enabled;
        if (g_config.general.texture_dump_enabled) {
            xemu_texture_packs_rebuild_dump_index();
        }
        xemu_queue_notification(
            g_config.general.texture_dump_enabled ?
                "Texture dumping: STARTED" :
                "Texture dumping: STOPPED");
    }
}

void FeatureTexturePacksRenderCommit()
{
    if (!g_reload_pending) {
        return;
    }

    xemu_texture_packs_rebuild_replacement_index();
    xemu_texture_packs_rebuild_dump_index();
    xemu_texture_packs_request_cache_flush();
    g_reload_pending = false;
}

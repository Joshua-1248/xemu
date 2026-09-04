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
#include "xemu-features/shared/detachable-windows.hh"

extern "C" {
#include "xemu-features/texture-packs/texture-packs.h"
}

#include <stdio.h>

/* Owned entirely by this feature. The generic HUD renderer only calls the
 * narrow commit hook after drawing. */
static bool g_reload_pending;
static bool g_texture_packs_window_open;
static constexpr const char *kTexturePacksDetachId = "texture-packs.window";

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

static void DrawTexturePacksSettingsBody()
{
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

    XemuTexturePacksMaterialConfig material_cfg;
    xemu_texture_packs_get_material_config(&material_cfg);
    bool material_changed = false;
    bool material_binding_change = false;

    ImGui::Separator();
    ImGui::TextUnformatted("Replacement Material Enhancement");
    ImGui::SameLine();
    if (ImGui::SmallButton("Defaults##material")) {
        material_cfg.flip_normal_y = false;
        material_cfg.normal_strength = 1.0f;
        material_cfg.ambient_strength = 0.20f;
        material_cfg.diffuse_strength = 1.00f;
        material_cfg.specular_strength = 0.35f;
        material_cfg.specular_power = 32.0f;
        material_cfg.parallax_scale = 0.02f;
        material_cfg.ao_strength = 1.00f;
        material_cfg.light_mode =
            XEMU_TEXTURE_PACKS_MATERIAL_LIGHT_HEADLIGHT;
        material_cfg.light_dir[0] = 0.35f;
        material_cfg.light_dir[1] = -0.35f;
        material_cfg.light_dir[2] = 0.87f;
        material_changed = true;
    }

    if (Toggle("Enable material-map enhancement", &material_cfg.enabled,
               "Apply game-agnostic enhancement lighting to replacement textures using optional _n/_s/_d/_ao sidecars")) {
        material_changed = true;
        material_binding_change = true;
    }

    if (material_cfg.enabled) {
        ImGui::Indent();

        int light_mode = material_cfg.light_mode;
        if (ImGui::Combo("Light mode", &light_mode,
                         "Camera-reactive headlight\0Directional\0")) {
            material_cfg.light_mode = light_mode;
            material_changed = true;
        }

        if (ImGui::SliderFloat("Normal strength", &material_cfg.normal_strength,
                               0.0f, 4.0f, "%.2f")) {
            material_changed = true;
        }
        if (ImGui::SliderFloat("Ambient", &material_cfg.ambient_strength,
                               0.0f, 2.0f, "%.2f")) {
            material_changed = true;
        }
        if (ImGui::SliderFloat("Diffuse", &material_cfg.diffuse_strength,
                               0.0f, 4.0f, "%.2f")) {
            material_changed = true;
        }
        if (ImGui::SliderFloat("Specular", &material_cfg.specular_strength,
                               0.0f, 4.0f, "%.2f")) {
            material_changed = true;
        }
        if (ImGui::SliderFloat("Gloss power", &material_cfg.specular_power,
                               1.0f, 128.0f, "%.1f")) {
            material_changed = true;
        }
        if (ImGui::SliderFloat("Parallax scale", &material_cfg.parallax_scale,
                               0.0f, 0.10f, "%.3f")) {
            material_changed = true;
        }
        if (ImGui::SliderFloat("AO strength", &material_cfg.ao_strength,
                               0.0f, 1.0f, "%.2f")) {
            material_changed = true;
        }

        bool flip_y = material_cfg.flip_normal_y;
        if (Toggle("Flip normal Y (OpenGL-style maps)", &flip_y,
                   "Enable this when a normal map looks inverted because its green channel was authored for OpenGL instead of DirectX")) {
            material_cfg.flip_normal_y = flip_y;
            material_changed = true;
        }

        if (material_cfg.light_mode == XEMU_TEXTURE_PACKS_MATERIAL_LIGHT_DIRECTIONAL) {
            if (ImGui::SliderFloat("Light X", &material_cfg.light_dir[0],
                                   -1.0f, 1.0f, "%.2f")) {
                material_changed = true;
            }
            if (ImGui::SliderFloat("Light Y", &material_cfg.light_dir[1],
                                   -1.0f, 1.0f, "%.2f")) {
                material_changed = true;
            }
            if (ImGui::SliderFloat("Light Z", &material_cfg.light_dir[2],
                                   -1.0f, 1.0f, "%.2f")) {
                material_changed = true;
            }
        }

        ImGui::Unindent();
    }

    if (material_changed) {
        xemu_texture_packs_set_material_config(&material_cfg);
        if (material_binding_change) {
            g_reload_pending = true;
        }
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

void FeatureTexturePacksDrawSettings()
{
    // Texture Packs now lives in a standalone Misc tool window. Keep the
    // native settings hook as a compatibility no-op so no upstream UI source
    // needs to change.
}

void FeatureTexturePacksDrawMiscMenuItem()
{
    ImGui::MenuItem("Texture Packs", nullptr, &g_texture_packs_window_open);
}

void FeatureTexturePacksDrawWindow()
{
    xemu_feature_detach::Register(kTexturePacksDetachId, "Texture Packs",
                                  &g_texture_packs_window_open,
                                  []() { FeatureTexturePacksDrawWindow(); });
    xemu_feature_detach::Pump();

    if (!g_texture_packs_window_open ||
        !xemu_feature_detach::ShouldDraw(kTexturePacksDetachId)) {
        return;
    }

    if (xemu_feature_detach::IsDetachedPass(kTexturePacksDetachId)) {
        xemu_feature_detach::PrepareWindow(kTexturePacksDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 700.0f),
                                 ImGuiCond_FirstUseEver);
    }
    const ImGuiWindowFlags flags =
        xemu_feature_detach::WindowFlags(kTexturePacksDetachId, 0);
    if (!ImGui::Begin("Texture Packs", &g_texture_packs_window_open, flags)) {
        ImGui::End();
        return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kTexturePacksDetachId);
    DrawTexturePacksSettingsBody();
    ImGui::End();
}

bool FeatureTexturePacksWindowOpen()
{
    return g_texture_packs_window_open;
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

//
// xemu custom fork - isolated audio pack frontend
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
#include "xemu-features/audio-packs/audio-packs.h"
}

#ifdef _WIN32
#pragma push_macro("close")
#undef close
#endif
#include <fstream>
#ifdef _WIN32
#pragma pop_macro("close")
#endif
#include <string>
#include <cstdlib>
#include <stdio.h>

namespace {

struct AudioPackHotkeys {
    int dump = 0;
    int replacements = 0;
    int reload = 0;
    bool dump_static = true;
    bool dump_streams = true;
    bool loaded = false;
};

AudioPackHotkeys g_audio_hotkeys;
bool g_audio_packs_window_open;
constexpr const char *kAudioPacksDetachId = "audio-packs.window";

static std::string AudioHotkeyPath()
{
    const char *base_path = xemu_settings_get_base_path();
    std::string base = base_path ? base_path : "";
    if (!base.empty() && base.back() != '/' && base.back() != '\\') {
#ifdef _WIN32
        base.push_back('\\');
#else
        base.push_back('/');
#endif
    }
    return base + "audio-packs-hotkeys.ini";
}

static bool IsValidImGuiHotkey(int key)
{
    return key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END;
}

static void LoadAudioHotkeys()
{
    if (g_audio_hotkeys.loaded) {
        return;
    }
    g_audio_hotkeys.loaded = true;

    std::ifstream in(AudioHotkeyPath());
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value_text = line.substr(eq + 1);
        char *end = nullptr;
        long parsed = std::strtol(value_text.c_str(), &end, 10);
        if (!end || *end != '\0') continue;
        int value = (int)parsed;
        if (key == "dump_static") {
            g_audio_hotkeys.dump_static = value != 0;
            continue;
        }
        if (key == "dump_streams") {
            g_audio_hotkeys.dump_streams = value != 0;
            continue;
        }
        if (!IsValidImGuiHotkey(value)) continue;
        if (key == "toggle_dump") g_audio_hotkeys.dump = value;
        else if (key == "toggle_replacements") g_audio_hotkeys.replacements = value;
        else if (key == "reload_replacements") g_audio_hotkeys.reload = value;
    }
    xemu_audio_packs_set_dump_categories(g_audio_hotkeys.dump_static,
                                         g_audio_hotkeys.dump_streams);
}

static void SaveAudioHotkeys()
{
    LoadAudioHotkeys();
    std::ofstream out(AudioHotkeyPath(), std::ios::trunc);
    if (!out) {
        fprintf(stderr, "audio-packs: could not save hotkeys\n");
        return;
    }
    out << "toggle_dump=" << g_audio_hotkeys.dump << '\n'
        << "toggle_replacements=" << g_audio_hotkeys.replacements << '\n'
        << "reload_replacements=" << g_audio_hotkeys.reload << '\n'
        << "dump_static=" << (g_audio_hotkeys.dump_static ? 1 : 0) << '\n'
        << "dump_streams=" << (g_audio_hotkeys.dump_streams ? 1 : 0) << '\n';
}

static bool HotkeyBinder(const char *label, int *key_value)
{
    static const char *capturing_label = nullptr;
    bool capturing = capturing_label == label;
    bool changed = false;

    const char *key_name = "(unset)";
    if (!capturing && IsValidImGuiHotkey(*key_value)) {
        const char *n = ImGui::GetKeyName((ImGuiKey)*key_value);
        if (n && n[0]) key_name = n;
    }

    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);

    char button[128];
    snprintf(button, sizeof(button), "%s##bind",
             capturing ? "Press a key..." : key_name);
    if (ImGui::Button(button, ImVec2(ImGui::GetWindowWidth() * 0.35f, 0))) {
        capturing_label = capturing ? nullptr : label;
        capturing = !capturing;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        *key_value = 0;
        capturing_label = nullptr;
        changed = true;
        capturing = false;
    }

    if (capturing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            capturing_label = nullptr;
        } else {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                if (ImGui::IsKeyPressed((ImGuiKey)k, false)) {
                    *key_value = k;
                    capturing_label = nullptr;
                    changed = true;
                    break;
                }
            }
        }
    }
    ImGui::PopID();
    return changed;
}

static bool HotkeyPressed(int key)
{
    return IsValidImGuiHotkey(key) &&
           ImGui::IsKeyPressed((ImGuiKey)key, false);
}

static void ToggleDumping()
{
    g_config.audio.dump_enabled = !g_config.audio.dump_enabled;
    xemu_audio_packs_refresh_paths();
    if (g_config.audio.dump_enabled) {
        xemu_audio_packs_rebuild_dump_index();
    } else {
        xemu_audio_packs_finish_stream_dumps();
    }
    xemu_queue_notification(g_config.audio.dump_enabled
                                ? "Source audio dumping: ON"
                                : "Source audio dumping: OFF");
}

static void ToggleReplacements()
{
    g_config.audio.replace_enabled = !g_config.audio.replace_enabled;
    xemu_audio_packs_rebuild_replacement_index();
    xemu_queue_notification(g_config.audio.replace_enabled
                                ? "Audio replacements: ON"
                                : "Audio replacements: OFF");
}

static void ReloadReplacements()
{
    xemu_audio_packs_rebuild_replacement_index();
    xemu_queue_notification("Reloaded audio replacements");
}

} // namespace

namespace {

static void DrawAudioPacksSettingsBody()
{
    LoadAudioHotkeys();

    if (Toggle("Dump source audio", &g_config.audio.dump_enabled,
               "Dump decoded source audio as logical WAV assets; exact loops are collapsed to one traversal with intro/loop/outro metadata")) {
        xemu_audio_packs_refresh_paths();
        if (g_config.audio.dump_enabled) {
            xemu_audio_packs_rebuild_dump_index();
        } else {
            xemu_audio_packs_finish_stream_dumps();
        }
    }

    ImGui::TextUnformatted("Dump categories");
    ImGui::Indent();
    bool category_changed = false;
    category_changed |= Toggle(
        "Static / buffered voices", &g_audio_hotkeys.dump_static,
        "Dump resident/static DirectSound buffers such as ordinary sound effects");
    category_changed |= Toggle(
        "Streaming SSL voices", &g_audio_hotkeys.dump_streams,
        "Dump and stitch streamed APU sources; exact repeated loop traversals are collapsed while unique intros and outros are preserved");
    ImGui::Unindent();
    if (category_changed) {
        xemu_audio_packs_set_dump_categories(g_audio_hotkeys.dump_static,
                                             g_audio_hotkeys.dump_streams);
        SaveAudioHotkeys();
    }

    ImGui::TextWrapped(
        "Loop-aware extraction is automatic. Looping WAVs include a standard "
        "RIFF smpl loop region and JSON intro/loop/outro frame metadata. "
        "Ambiguous repetition is never trimmed.");

    if (Toggle("Skip already-replaced audio when dumping",
               &g_config.audio.dump_skip_replaced,
               "Do not dump audio sources that already have a replacement")) {
        xemu_audio_packs_rebuild_replacement_index();
    }
    if (Toggle("Use audio replacements", &g_config.audio.replace_enabled,
               "Replace matching static and streaming APU source voices; use <hash>_1.wav, _2.wav, ... for random variants")) {
        xemu_audio_packs_rebuild_replacement_index();
    }

    FilePicker("Audio dump directory", g_config.audio.dump_dir,
               nullptr, 0, true, [](const char *path) {
                   xemu_settings_set_string(&g_config.audio.dump_dir, path);
                   xemu_audio_packs_refresh_paths();
               });
    FilePicker("Audio replacement directory", g_config.audio.replace_dir,
               nullptr, 0, true, [](const char *path) {
                   xemu_settings_set_string(&g_config.audio.replace_dir, path);
                   xemu_audio_packs_refresh_paths();
               });

    if (ImGui::Button("Reload audio replacements")) {
        ReloadReplacements();
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().WindowPadding.y));
    ImGui::TextUnformatted("Hotkeys");
    bool changed = false;
    changed |= HotkeyBinder("Toggle source audio dumping", &g_audio_hotkeys.dump);
    changed |= HotkeyBinder("Toggle audio replacements", &g_audio_hotkeys.replacements);
    changed |= HotkeyBinder("Reload audio replacements", &g_audio_hotkeys.reload);
    if (changed) {
        SaveAudioHotkeys();
    }
    ImGui::TextDisabled("Audio-pack hotkeys and dump-category choices are stored separately in audio-packs-hotkeys.ini.");

}

static void DrawAudioPacksInfo()
{
    ImGui::TextWrapped(
        "Dumping and replacement cover static hardware voices, software-fed/reused "
        "resident buffers, and packetized SSL streams. Exact whole-buffer and tiered "
        "stream-prefix matching remain the fast paths. A transport-agnostic "
        "consumed-source matcher also watches the decoded PCM actually passing through "
        "MCPX and can identify verified landmarks at arbitrary offsets inside "
        "reused/circular buffers.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "The guest's real CBO/SSL/ring consumption remains authoritative while aligned "
        "replacement samples are substituted afterward. Replacement WAVs used by "
        "passthrough matching are pre-normalized to the source rate off the APU worker. "
        "Local source dump WAVs build the arbitrary-offset landmark index when "
        "replacements are reloaded.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Multipass mixbin voices remain internal processing and are intentionally not "
        "treated as source assets. Use <hash>.wav for one replacement, or "
        "<hash>_1.wav, <hash>_2.wav, ... for random variants.");
}

} // namespace

void FeatureAudioPacksDrawSettings()
{
    // Audio Packs now lives in a standalone Misc tool window. Keep the native
    // settings hook as a compatibility no-op.
}

void FeatureAudioPacksDrawSettingsAfterQuality()
{
    // Compatibility no-op retained for older feature-owned integration code.
}

void FeatureAudioPacksDrawMiscMenuItem()
{
    ImGui::MenuItem("Audio Packs", nullptr, &g_audio_packs_window_open);
}

void FeatureAudioPacksDrawWindow()
{
    xemu_feature_detach::Register(kAudioPacksDetachId, "Audio Packs",
                                  &g_audio_packs_window_open,
                                  []() { FeatureAudioPacksDrawWindow(); });
    xemu_feature_detach::Pump();

    if (!g_audio_packs_window_open ||
        !xemu_feature_detach::ShouldDraw(kAudioPacksDetachId)) {
        return;
    }

    if (xemu_feature_detach::IsDetachedPass(kAudioPacksDetachId)) {
        xemu_feature_detach::PrepareWindow(kAudioPacksDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(640.0f, 690.0f),
                                 ImGuiCond_FirstUseEver);
    }
    const ImGuiWindowFlags flags =
        xemu_feature_detach::WindowFlags(kAudioPacksDetachId, 0);
    if (!ImGui::Begin("Audio Packs", &g_audio_packs_window_open, flags)) {
        ImGui::End();
        return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kAudioPacksDetachId);

    if (ImGui::BeginTabBar("##audio_packs_tabs")) {
        if (ImGui::BeginTabItem("Settings")) {
            DrawAudioPacksSettingsBody();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info")) {
            DrawAudioPacksInfo();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

bool FeatureAudioPacksWindowOpen()
{
    return g_audio_packs_window_open;
}

void FeatureAudioPacksProcessHotkeys(bool gameplay_has_focus)
{
    if (!gameplay_has_focus) {
        return;
    }
    LoadAudioHotkeys();

    if (HotkeyPressed(g_audio_hotkeys.dump)) {
        ToggleDumping();
    }
    if (HotkeyPressed(g_audio_hotkeys.replacements)) {
        ToggleReplacements();
    }
    if (HotkeyPressed(g_audio_hotkeys.reload)) {
        ReloadReplacements();
    }
}

#ifndef CONFIG_XEMU_FEATURE_FAST_FORWARD
// ui/xui/main.cc already owns one custom-feature hotkey hook. If Fast Forward
// is compiled out, Audio Packs supplies that hook so its hotkeys remain usable
// without adding another native Xemu call site.
void FeatureFastForwardUpdateHotkey(bool gameplay_has_focus)
{
    FeatureAudioPacksProcessHotkeys(gameplay_has_focus);
}
#endif

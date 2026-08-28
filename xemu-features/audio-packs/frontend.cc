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
#include "ui/xemu-settings.h"

extern "C" {
#include "xemu-features/audio-packs/audio-packs.h"
}

void FeatureAudioPacksDrawSettings()
{
    SectionTitle("Audio Packs");
    if (Toggle("Dump source audio", &g_config.audio.dump_enabled,
               "Dump decoded static APU voice sources to WAV as they are used")) {
        xemu_audio_packs_refresh_paths();
        if (g_config.audio.dump_enabled) {
            xemu_audio_packs_rebuild_dump_index();
        }
    }
    if (Toggle("Skip already-replaced audio when dumping",
               &g_config.audio.dump_skip_replaced,
               "Do not dump audio sources that already have a replacement")) {
        xemu_audio_packs_rebuild_replacement_index();
    }
    if (Toggle("Use audio replacements", &g_config.audio.replace_enabled,
               "Replace matching static APU voices; use <hash>_1.wav, _2.wav, ... for random variants")) {
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
        xemu_audio_packs_rebuild_replacement_index();
    }
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().WindowPadding.y));
    ImGui::TextWrapped(
        "Static hardware voices are supported in this first pass. Use <hash>.wav "
        "for one replacement, or <hash>_1.wav, <hash>_2.wav, ... to choose one "
        "randomly each time that sound starts. Replacement WAVs are preloaded when "
        "the pack is indexed so disk/decode work never blocks APU voice workers. "
        "Replacement WAVs may be shorter or longer than the original; their "
        "natural duration controls voice completion while Xbox pitch, volume, "
        "envelopes, filters, HRTF and DSP routing remain active. Streaming SSL "
        "voices are left untouched for the next phase.");
}

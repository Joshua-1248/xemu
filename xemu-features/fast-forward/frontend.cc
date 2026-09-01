// SPDX-License-Identifier: GPL-2.0-or-later
//
// xemu custom fork - isolated fast-forward frontend
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
#include "xemu-features/audio-packs/frontend.hh"
#include "xemu-features/tas/tas.h"

extern "C" {
#include "xemu-features/fast-forward/fast-forward.h"
#include "system/runstate.h"
}

#include <stdio.h>

/*
 * Host pacing state is a single atomic mode:
 *   0 = Unlimited, 1 = inactive, 2..5 = multiplier.
 * Hardware-facing call sites only query the C API and never depend on UI
 * implementation details.
 */
static gint g_fast_forward_mode = 1;

static int normalize_fast_forward_mode(bool active)
{
    if (!active) {
        return 1;
    }
    int multiplier = g_config.general.fast_forward_multiplier;
    if (multiplier == 0) {
        return 0;
    }
    return CLAMP(multiplier, 2, 5);
}

extern "C" int xemu_fast_forward_mode(void)
{
    return g_atomic_int_get(&g_fast_forward_mode);
}

extern "C" bool xemu_fast_forward_active(void)
{
    return xemu_fast_forward_mode() != 1;
}

extern "C" bool xemu_fast_forward_unlimited(void)
{
    return xemu_fast_forward_mode() == 0;
}

extern "C" bool xemu_fast_forward_can_unblock_main_loop(void)
{
    // Unlimited is intentionally unthrottled by the APU, but do not force the
    // QEMU main loop into a permanent zero-timeout spin. That spin competes
    // with the vCPU/UI threads and was the unstable path behind Unlimited
    // crashes. The feature timing layer supplies a tiny bounded VBLANK/timer
    // cadence instead, which remains effectively unlimited on real hosts.
    return false;
}

extern "C" int xemu_fast_forward_multiplier(void)
{
    int mode = xemu_fast_forward_mode();
    return mode >= 2 ? mode : 1;
}

extern "C" void xemu_fast_forward_set_active(bool active)
{
    const int old_mode = xemu_fast_forward_mode();

    /* Strict TAS determinism owns guest pacing. Do not let a gameplay hotkey,
     * toggle-mode state, or settings refresh silently re-enable host-paced
     * Fast Forward while a deterministic movie is active. */
    if (active && xemu_tas_deterministic_mode()) {
        if (old_mode != 1) {
            g_atomic_int_set(&g_fast_forward_mode, 1);
        }
        return;
    }
    const bool old_active = old_mode != 1;
    const int new_mode = normalize_fast_forward_mode(active);

    if (old_mode == new_mode) {
        return;
    }

    g_atomic_int_set(&g_fast_forward_mode, new_mode);

    if (old_active == active) {
        return;
    }

    if (active) {
        if (new_mode == 0) {
            xemu_queue_notification("Fast Forward: Unlimited");
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Fast Forward: %dx", new_mode);
            xemu_queue_notification(msg);
        }
    } else {
        xemu_queue_notification("Fast Forward: OFF");
    }
}

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

void FeatureFastForwardDrawSettings()
{
    SectionTitle("Fast Forward");

    int ff_speed = g_config.general.fast_forward_multiplier;
    int ff_speed_index;

    if (ff_speed == 0) {
        ff_speed_index = 4;
    } else {
        ff_speed = CLAMP(ff_speed, 2, 5);
        ff_speed_index = ff_speed - 2;
    }

    if (ChevronCombo("Fast-forward speed", &ff_speed_index,
                     "2x\0"
                     "3x\0"
                     "4x\0"
                     "5x\0"
                     "Unlimited\0",
                     "Select emulation speed while fast-forward is active")) {
        g_config.general.fast_forward_multiplier =
            (ff_speed_index == 4) ? 0 : (ff_speed_index + 2);

        if (xemu_fast_forward_active()) {
            xemu_fast_forward_set_active(true);
        }
    }

    Toggle("Toggle mode",
           &g_config.general.fast_forward_toggle_mode,
           "Off: hold the hotkey. On: press once to enable/disable.");

    Toggle("Preserve audio pitch",
           &g_config.general.fast_forward_preserve_pitch,
           "Keep fast-forward audio near its normal pitch instead of "
           "raising pitch with speed. Experimental granular time-compression.");

    HotkeyBinder("Fast-forward hotkey", &g_config.general.fast_forward_hotkey);

    ImGui::TextWrapped(
        "Fast-forward audio remains enabled. Preserve audio pitch uses "
        "experimental granular time-compression.");
}

void FeatureFastForwardUpdateHotkey(bool gameplay_has_focus)
{
    // Reuse the existing custom-feature hotkey call site for Audio Packs too;
    // this keeps all new behavior out of native ui/xui/main.cc.
    FeatureAudioPacksProcessHotkeys(gameplay_has_focus);

    int ff_key_value = g_config.general.fast_forward_hotkey;
    bool ff_key_valid =
        ff_key_value >= ImGuiKey_NamedKey_BEGIN &&
        ff_key_value < ImGuiKey_NamedKey_END;

    if (!gameplay_has_focus) {
        if (!g_config.general.fast_forward_toggle_mode) {
            xemu_fast_forward_set_active(false);
        }
        return;
    }

    if (ff_key_valid) {
        ImGuiKey ff_key = (ImGuiKey)ff_key_value;
        if (g_config.general.fast_forward_toggle_mode) {
            if (ImGui::IsKeyPressed(ff_key, false)) {
                xemu_fast_forward_set_active(!xemu_fast_forward_active());
            }
        } else {
            xemu_fast_forward_set_active(ImGui::IsKeyDown(ff_key));
        }
    } else if (!g_config.general.fast_forward_toggle_mode) {
        xemu_fast_forward_set_active(false);
    }
}

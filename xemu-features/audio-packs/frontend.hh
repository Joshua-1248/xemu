// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - Audio Packs frontend boundary
#pragma once
#include "config-host.h"

#ifdef __cplusplus
#include <cstring>
#include "ui/xui/widgets.hh"
#endif

#ifdef CONFIG_XEMU_FEATURE_AUDIO_PACKS
void FeatureAudioPacksDrawSettings();
void FeatureAudioPacksDrawSettingsAfterQuality();
void FeatureAudioPacksProcessHotkeys(bool gameplay_has_focus);
#else
static inline void FeatureAudioPacksDrawSettings() {}
static inline void FeatureAudioPacksDrawSettingsAfterQuality() {}
static inline void FeatureAudioPacksProcessHotkeys(bool) {}
#endif

#ifdef __cplusplus
#ifdef CONFIG_XEMU_FEATURE_AUDIO_PACKS
/*
 * Feature-only settings integration shim.
 *
 * Native MainMenuAudioView currently invokes the Audio Packs hook immediately
 * before its native Quality section.  The user wants Audio Packs to follow
 * Quality (the same way Texture Packs follows Display's native controls), but
 * native ui/xui/main-menu.cc is intentionally off-limits.  Defer the feature
 * draw at the existing hook and flush it immediately after the final native
 * Quality toggle instead.
 *
 * This wrapper is transparent for every other Toggle() call.
 */
static inline bool FeatureAudioPacksToggleBridge(const char *label, bool *value,
                                                  const char *description = nullptr)
{
    bool changed = ::Toggle(label, value, description);
    if (label && std::strcmp(label, "DSP JIT engine") == 0) {
        FeatureAudioPacksDrawSettingsAfterQuality();
    }
    return changed;
}
#define Toggle(...) FeatureAudioPacksToggleBridge(__VA_ARGS__)
#endif

#endif

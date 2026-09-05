// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - Audio Packs frontend boundary
#pragma once
#include "config-host.h"

#ifdef CONFIG_XEMU_FEATURE_AUDIO_PACKS
void FeatureAudioPacksDrawSettings();
void FeatureAudioPacksDrawSettingsAfterQuality();
void FeatureAudioPacksDrawMiscMenuItem();
void FeatureAudioPacksDrawWindow();
bool FeatureAudioPacksWindowOpen();
void FeatureAudioPacksProcessHotkeys(bool gameplay_has_focus);
#else
static inline void FeatureAudioPacksDrawSettings() {}
static inline void FeatureAudioPacksDrawSettingsAfterQuality() {}
static inline void FeatureAudioPacksDrawMiscMenuItem() {}
static inline void FeatureAudioPacksDrawWindow() {}
static inline bool FeatureAudioPacksWindowOpen() { return false; }
static inline void FeatureAudioPacksProcessHotkeys(bool) {}
#endif

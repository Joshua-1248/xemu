// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - Fast Forward frontend boundary
#pragma once
#include "config-host.h"

#ifdef CONFIG_XEMU_FEATURE_FAST_FORWARD
void FeatureFastForwardDrawSettings();
#else
static inline void FeatureFastForwardDrawSettings() {}
#endif

// ui/xui/main.cc already owns this narrow custom-feature hotkey hook. Audio
// Packs shares it when present so no additional native-Xemu integration point
// is required.
#if defined(CONFIG_XEMU_FEATURE_FAST_FORWARD) || defined(CONFIG_XEMU_FEATURE_AUDIO_PACKS)
void FeatureFastForwardUpdateHotkey(bool gameplay_has_focus);
#else
static inline void FeatureFastForwardUpdateHotkey(bool) {}
#endif

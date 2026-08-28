// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - Fast Forward frontend boundary
#pragma once
#include "config-host.h"
#ifdef CONFIG_XEMU_FEATURE_FAST_FORWARD
void FeatureFastForwardDrawSettings();
void FeatureFastForwardUpdateHotkey(bool gameplay_has_focus);
#else
static inline void FeatureFastForwardDrawSettings() {}
static inline void FeatureFastForwardUpdateHotkey(bool) {}
#endif

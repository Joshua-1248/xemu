// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "config-host.h"
#ifdef CONFIG_XEMU_FEATURE_DISC_MODDING
void FeatureDiscModdingDrawMiscMenuItem();
void FeatureDiscModdingDrawWindow();
bool FeatureDiscModdingWindowOpen();
#else
static inline void FeatureDiscModdingDrawMiscMenuItem() {}
static inline void FeatureDiscModdingDrawWindow() {}
static inline bool FeatureDiscModdingWindowOpen() { return false; }
#endif

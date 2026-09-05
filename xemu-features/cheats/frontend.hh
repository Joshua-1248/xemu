// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - Cheats/Patches standalone frontend boundary
#pragma once
#include "config-host.h"

#ifdef CONFIG_XEMU_FEATURE_CHEATS
void FeatureCodesDrawMiscMenuItem();
void FeatureCodesShowWindows();
bool FeatureCodesWindowsOpen();
void FeatureCodesOpenWindow();
#else
static inline void FeatureCodesDrawMiscMenuItem() {}
static inline void FeatureCodesShowWindows() {}
static inline bool FeatureCodesWindowsOpen() { return false; }
static inline void FeatureCodesOpenWindow() {}
#endif

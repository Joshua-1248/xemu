// xemu custom fork - Scripting frontend boundary
#pragma once
#include "config-host.h"
#ifdef CONFIG_XEMU_FEATURE_SCRIPTING
void FeatureScriptToolsDrawMenu();
void FeatureScriptToolsShowWindows();
bool FeatureScriptToolsWindowsOpen();
#else
static inline void FeatureScriptToolsDrawMenu() {}
static inline void FeatureScriptToolsShowWindows() {}
static inline bool FeatureScriptToolsWindowsOpen() { return false; }
#endif

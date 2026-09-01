// xemu custom fork - Misc feature menu/window aggregation boundary
#pragma once
#include "config-host.h"

// ui/xui/menubar.cc and ui/xui/main.cc already call these narrow hooks. Keep
// them available when either Scripting or Cheats is enabled so the Misc menu
// and its standalone windows remain independent without adding native-Xemu
// integration points.
#if defined(CONFIG_XEMU_FEATURE_SCRIPTING) || defined(CONFIG_XEMU_FEATURE_CHEATS)
void FeatureScriptToolsDrawMenu();
void FeatureScriptToolsShowWindows();
bool FeatureScriptToolsWindowsOpen();
#else
static inline void FeatureScriptToolsDrawMenu() {}
static inline void FeatureScriptToolsShowWindows() {}
static inline bool FeatureScriptToolsWindowsOpen() { return false; }
#endif

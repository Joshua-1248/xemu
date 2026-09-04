// xemu custom fork - Misc feature menu/window aggregation boundary
#pragma once
#include "config-host.h"

// ui/xui/menubar.cc and ui/xui/main.cc already call these narrow hooks. Keep
// one aggregation boundary even when Scripting itself is disabled, so Texture
// Packs, Audio Packs, Fast Forward, Free Camera and Geometry Dumper do not
// depend on either scripting or cheats merely to be reachable from Misc.
#if defined(CONFIG_XEMU_FEATURE_SCRIPTING) || defined(CONFIG_XEMU_FEATURE_CHEATS)
void FeatureScriptToolsDrawMenu();
void FeatureScriptToolsShowWindows();
bool FeatureScriptToolsWindowsOpen();
#elif defined(CONFIG_XEMU_FEATURE_TEXTURE_PACKS) || \
      defined(CONFIG_XEMU_FEATURE_AUDIO_PACKS) || \
      defined(CONFIG_XEMU_FEATURE_FAST_FORWARD) || \
      defined(CONFIG_XEMU_FEATURE_DEBUG_TOOLS) || \
      defined(CONFIG_XEMU_FEATURE_DISC_MODDING)
#include "ui/xui/common.hh"
#include "xemu-features/shared/misc-menu.hh"
static inline void FeatureScriptToolsDrawMenu()
{
    if (ImGui::BeginMenu("Misc")) {
        FeatureCustomToolsDrawMiscMenuItems();
        ImGui::EndMenu();
    }
}
static inline void FeatureScriptToolsShowWindows()
{
    FeatureCustomToolsShowWindows();
}
static inline bool FeatureScriptToolsWindowsOpen()
{
    return FeatureCustomToolsWindowsOpen();
}
#else
static inline void FeatureScriptToolsDrawMenu() {}
static inline void FeatureScriptToolsShowWindows() {}
static inline bool FeatureScriptToolsWindowsOpen() { return false; }
#endif

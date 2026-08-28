// xemu custom fork - optional disassembler/debug extension boundary
#pragma once
#include "config-host.h"
#ifdef CONFIG_XEMU_FEATURE_DEBUG_TOOLS
void FeatureDebugToolsDrawMenuItems();
void FeatureDebugToolsDrawWindows();
#else
static inline void FeatureDebugToolsDrawMenuItems() {}
static inline void FeatureDebugToolsDrawWindows() {}
#endif

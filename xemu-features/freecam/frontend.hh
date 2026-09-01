// xemu custom fork - Free Camera frontend boundary
#pragma once
#include "config-host.h"

#if defined(CONFIG_XEMU_FEATURE_DEBUG_TOOLS)
void FeatureFreecamDrawMiscMenuItem();
void FeatureFreecamDrawWindow();
#else
static inline void FeatureFreecamDrawMiscMenuItem() {}
static inline void FeatureFreecamDrawWindow() {}
#endif

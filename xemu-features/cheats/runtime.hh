// xemu custom fork - Cheats runtime boundary
#pragma once
#include "config-host.h"
#ifdef CONFIG_XEMU_FEATURE_CHEATS
void FeatureCodesTick();
#else
static inline void FeatureCodesTick() {}
#endif

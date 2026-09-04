// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - Geometry Dumper frontend boundary
#pragma once
#include "config-host.h"

#if defined(CONFIG_XEMU_FEATURE_DEBUG_TOOLS)
void FeatureGeometryDumperDrawMenuItem();
void FeatureGeometryDumperDrawWindow();
bool FeatureGeometryDumperWindowOpen();
#else
static inline void FeatureGeometryDumperDrawMenuItem() {}
static inline void FeatureGeometryDumperDrawWindow() {}
static inline bool FeatureGeometryDumperWindowOpen() { return false; }
#endif

// SPDX-License-Identifier: GPL-2.0-or-later
//
// Optional Debug Tools -> Cheats integration boundary.
//
// Kept neutral when the Cheats feature is disabled so Debug Tools remains an
// independently buildable custom feature.  The payload is a list of existing
// raw cheat-code command/value pairs.  Reserved Type F is not special-cased or
// repurposed by this bridge.
#pragma once

#include "config-host.h"
#include <cstddef>
#include <cstdint>

#ifdef CONFIG_XEMU_FEATURE_CHEATS
bool FeatureCodesAddGeneratedAsmCheat(const char *name, const char *desc,
                                      const uint32_t *cmds,
                                      const uint32_t *vals,
                                      size_t count, bool enabled);
#else
static inline bool FeatureCodesAddGeneratedAsmCheat(const char *, const char *,
                                                     const uint32_t *,
                                                     const uint32_t *,
                                                     size_t, bool)
{
    return false;
}
#endif

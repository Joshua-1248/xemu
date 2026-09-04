// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - CHD frontend path helper
#pragma once

#include "config-host.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool xemu_chd_support_enabled(void)
{
#ifdef CONFIG_XEMU_FEATURE_CHD
    return true;
#else
    return false;
#endif
}

static inline bool xemu_chd_path_is_chd(const char *path)
{
#ifdef CONFIG_XEMU_FEATURE_CHD
    if (!path) {
        return false;
    }
    const char *dot = strrchr(path, '.');
    if (!dot || strlen(dot) != 4) {
        return false;
    }
    return dot[1] && dot[2] && dot[3] &&
           (dot[1] == 'c' || dot[1] == 'C') &&
           (dot[2] == 'h' || dot[2] == 'H') &&
           (dot[3] == 'd' || dot[3] == 'D');
#else
    (void)path;
    return false;
#endif
}

static inline const char *xemu_chd_block_format_for_path(const char *path)
{
    return xemu_chd_path_is_chd(path) ? "chd" : "raw";
}

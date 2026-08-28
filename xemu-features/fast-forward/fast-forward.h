/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xemu custom fork - optional Fast Forward public state API. */
#ifndef XEMU_FAST_FORWARD_H
#define XEMU_FAST_FORWARD_H

#include "config-host.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_XEMU_FEATURE_FAST_FORWARD
bool xemu_fast_forward_active(void);
bool xemu_fast_forward_unlimited(void);
bool xemu_fast_forward_can_unblock_main_loop(void);
int xemu_fast_forward_multiplier(void);
/* 1 = normal/inactive, 0 = Unlimited, 2..5 = fixed multiplier. */
int xemu_fast_forward_mode(void);
void xemu_fast_forward_set_active(bool active);
#else
static inline bool xemu_fast_forward_active(void) { return false; }
static inline bool xemu_fast_forward_unlimited(void) { return false; }
static inline bool xemu_fast_forward_can_unblock_main_loop(void) { return false; }
static inline int xemu_fast_forward_multiplier(void) { return 1; }
static inline int xemu_fast_forward_mode(void) { return 1; }
static inline void xemu_fast_forward_set_active(bool active) { (void)active; }
#endif

#ifdef __cplusplus
}
#endif
#endif

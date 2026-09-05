/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XEMU_FEATURES_VOLUME_AMPLIFIER_H
#define XEMU_FEATURES_VOLUME_AMPLIFIER_H
#include "config-host.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct SDL_AudioStream;
#ifdef CONFIG_XEMU_FEATURE_VOLUME_AMPLIFIER
bool xemu_volume_amplifier_apply(struct SDL_AudioStream *stream, double volume);
void xemu_volume_amplifier_reset(struct SDL_AudioStream *stream);
bool xemu_volume_amplifier_is_muted(void);
bool xemu_volume_amplifier_toggle_mute(void);
static inline float xemu_volume_amplifier_max(void) { return 3.0f; }
#else
static inline bool xemu_volume_amplifier_apply(struct SDL_AudioStream *stream, double volume)
{ (void)stream; (void)volume; return false; }
static inline void xemu_volume_amplifier_reset(struct SDL_AudioStream *stream) { (void)stream; }
static inline bool xemu_volume_amplifier_is_muted(void) { return false; }
static inline bool xemu_volume_amplifier_toggle_mute(void) { return false; }
static inline float xemu_volume_amplifier_max(void) { return 1.0f; }
#endif
#ifdef __cplusplus
}
#endif
#endif

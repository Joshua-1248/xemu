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
static inline float xemu_volume_amplifier_max(void) { return 2.0f; }
#else
static inline bool xemu_volume_amplifier_apply(struct SDL_AudioStream *stream, double volume)
{ (void)stream; (void)volume; return false; }
static inline void xemu_volume_amplifier_reset(struct SDL_AudioStream *stream) { (void)stream; }
static inline float xemu_volume_amplifier_max(void) { return 1.0f; }
#endif
#ifdef __cplusplus
}
#endif
#endif

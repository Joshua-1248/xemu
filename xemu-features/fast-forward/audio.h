#ifndef XEMU_FEATURES_FAST_FORWARD_AUDIO_H
#define XEMU_FEATURES_FAST_FORWARD_AUDIO_H
#include "config-host.h"
#include <stdbool.h>
#include <stdint.h>
struct SDL_AudioStream;
#ifdef __cplusplus
extern "C" {
#endif
#ifdef CONFIG_XEMU_FEATURE_FAST_FORWARD
bool xemu_fast_forward_audio_submit(struct SDL_AudioStream *stream,
                                    const int16_t input[256][2],
                                    int queued_bytes_high);
void xemu_fast_forward_audio_reset(struct SDL_AudioStream *stream);
#else
static inline bool xemu_fast_forward_audio_submit(struct SDL_AudioStream *stream,
                                                   const int16_t input[256][2],
                                                   int queued_bytes_high)
{ (void)stream; (void)input; (void)queued_bytes_high; return false; }
static inline void xemu_fast_forward_audio_reset(struct SDL_AudioStream *stream) { (void)stream; }
#endif
#ifdef __cplusplus
}
#endif
#endif

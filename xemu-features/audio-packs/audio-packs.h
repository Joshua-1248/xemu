/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xemu custom fork - Original Xbox APU source audio dump/replacement */
#ifndef XEMU_FEATURES_AUDIO_PACKS_H
#define XEMU_FEATURES_AUDIO_PACKS_H
#include "config-host.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef CONFIG_XEMU_FEATURE_AUDIO_PACKS
bool xemu_audio_packs_enabled(void);
bool xemu_audio_packs_should_prepare_voice(void);
bool xemu_audio_packs_should_prepare_static_voice(void);
bool xemu_audio_packs_should_prepare_stream_voice(void);
bool xemu_audio_packs_should_dump_streams(void);
void xemu_audio_packs_set_dump_categories(bool dump_static, bool dump_streams);
void xemu_audio_packs_finish_stream_dumps(void);
void xemu_audio_packs_cancel_stream_dumps(void);
void xemu_audio_packs_refresh_paths(void);
void xemu_audio_packs_rebuild_replacement_index(void);
void xemu_audio_packs_rebuild_dump_index(void);
bool xemu_audio_packs_replacements_available(void);
void xemu_audio_packs_init(void);
void xemu_audio_packs_frame_sync(void);
void xemu_audio_packs_finalize(void);
void xemu_audio_packs_reset(void);
void xemu_audio_packs_voice_reset(unsigned int voice);
void xemu_audio_packs_voice_mark_unsupported(unsigned int voice);
bool xemu_audio_packs_prepare_static_voice(unsigned int voice, const int16_t *pcm,
                                            uint32_t frames, unsigned int channels,
                                            uint32_t observed_source_rate, bool loop,
                                            uint32_t loop_start, const char *source_format);
bool xemu_audio_packs_stream_segment_needed(unsigned int voice,
                                            uint64_t segment_signature,
                                            uint32_t live_cbo);
bool xemu_audio_packs_stream_append_segment(unsigned int voice, const int16_t *pcm,
                                            uint32_t frames, unsigned int channels,
                                            uint32_t sample_rate, uint64_t segment_signature,
                                            uint32_t live_cbo, const char *source_format);
void xemu_audio_packs_stream_voice_idle(unsigned int voice);
bool xemu_audio_packs_stream_override_samples(unsigned int voice,
                                              float samples[][2], int count);
bool xemu_audio_packs_process_consumed_source(unsigned int voice,
                                              float samples[][2], int count);
bool xemu_audio_packs_voice_prepared(unsigned int voice);
bool xemu_audio_packs_static_voice_retry_needed(unsigned int voice);
bool xemu_audio_packs_voice_has_replacement(unsigned int voice);
void xemu_audio_packs_note_guest_cbo_write(unsigned int voice, uint32_t guest_cbo);
bool xemu_audio_packs_voice_apply_guest_retrigger(unsigned int voice, uint32_t live_guest_cbo);
int xemu_audio_packs_voice_get_samples(unsigned int voice, float samples[][2], int requested);
float xemu_audio_packs_voice_rate_scale(unsigned int voice);
uint32_t xemu_audio_packs_voice_guest_cbo(unsigned int voice);
bool xemu_audio_packs_voice_finished(unsigned int voice);
#else
static inline bool xemu_audio_packs_enabled(void) { return false; }
static inline bool xemu_audio_packs_should_prepare_voice(void) { return false; }
static inline bool xemu_audio_packs_should_prepare_static_voice(void) { return false; }
static inline bool xemu_audio_packs_should_prepare_stream_voice(void) { return false; }
static inline bool xemu_audio_packs_should_dump_streams(void) { return false; }
static inline void xemu_audio_packs_set_dump_categories(bool dump_static, bool dump_streams)
{ (void)dump_static; (void)dump_streams; }
static inline void xemu_audio_packs_finish_stream_dumps(void) {}
static inline void xemu_audio_packs_cancel_stream_dumps(void) {}
static inline void xemu_audio_packs_refresh_paths(void) {}
static inline void xemu_audio_packs_rebuild_replacement_index(void) {}
static inline void xemu_audio_packs_rebuild_dump_index(void) {}
static inline bool xemu_audio_packs_replacements_available(void) { return false; }
static inline void xemu_audio_packs_init(void) {}
static inline void xemu_audio_packs_frame_sync(void) {}
static inline void xemu_audio_packs_finalize(void) {}
static inline void xemu_audio_packs_reset(void) {}
static inline void xemu_audio_packs_voice_reset(unsigned int voice) { (void)voice; }
static inline void xemu_audio_packs_voice_mark_unsupported(unsigned int voice) { (void)voice; }
static inline bool xemu_audio_packs_prepare_static_voice(unsigned int voice, const int16_t *pcm,
                                                          uint32_t frames, unsigned int channels,
                                                          uint32_t observed_source_rate, bool loop,
                                                          uint32_t loop_start, const char *source_format)
{ (void)voice; (void)pcm; (void)frames; (void)channels; (void)observed_source_rate; (void)loop; (void)loop_start; (void)source_format; return false; }
static inline bool xemu_audio_packs_stream_segment_needed(unsigned int voice,
                                                          uint64_t segment_signature,
                                                          uint32_t live_cbo)
{ (void)voice; (void)segment_signature; (void)live_cbo; return false; }
static inline bool xemu_audio_packs_stream_append_segment(unsigned int voice,
                                                          const int16_t *pcm,
                                                          uint32_t frames,
                                                          unsigned int channels,
                                                          uint32_t sample_rate,
                                                          uint64_t segment_signature,
                                                          uint32_t live_cbo,
                                                          const char *source_format)
{ (void)voice; (void)pcm; (void)frames; (void)channels; (void)sample_rate; (void)segment_signature; (void)live_cbo; (void)source_format; return false; }
static inline void xemu_audio_packs_stream_voice_idle(unsigned int voice)
{ (void)voice; }
static inline bool xemu_audio_packs_stream_override_samples(unsigned int voice,
                                                            float samples[][2],
                                                            int count)
{ (void)voice; (void)samples; (void)count; return false; }
static inline bool xemu_audio_packs_process_consumed_source(unsigned int voice,
                                                            float samples[][2],
                                                            int count)
{ (void)voice; (void)samples; (void)count; return false; }
static inline bool xemu_audio_packs_voice_prepared(unsigned int voice) { (void)voice; return false; }
static inline bool xemu_audio_packs_static_voice_retry_needed(unsigned int voice) { (void)voice; return false; }
static inline bool xemu_audio_packs_voice_has_replacement(unsigned int voice) { (void)voice; return false; }
static inline void xemu_audio_packs_note_guest_cbo_write(unsigned int voice, uint32_t guest_cbo)
{ (void)voice; (void)guest_cbo; }
static inline bool xemu_audio_packs_voice_apply_guest_retrigger(unsigned int voice, uint32_t live_guest_cbo)
{ (void)voice; (void)live_guest_cbo; return false; }
static inline int xemu_audio_packs_voice_get_samples(unsigned int voice, float samples[][2], int requested)
{ (void)voice; (void)samples; (void)requested; return -1; }
static inline float xemu_audio_packs_voice_rate_scale(unsigned int voice) { (void)voice; return 1.0f; }
static inline uint32_t xemu_audio_packs_voice_guest_cbo(unsigned int voice) { (void)voice; return 0; }
static inline bool xemu_audio_packs_voice_finished(unsigned int voice) { (void)voice; return false; }
#endif
#ifdef __cplusplus
}
#endif
#endif

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * MCPX/APU feature boundary associated with code derived from the xemu/QEMU
 * MCPX Audio Processing Unit implementation by espes, Jannik Vogel,
 * Matt Borgerson, and contributors.
 */
#ifndef XEMU_FEATURES_AUDIO_PACKS_APU_H
#define XEMU_FEATURES_AUDIO_PACKS_APU_H
#include "config-host.h"
#include <stdint.h>
struct MCPXAPUState;
#ifdef CONFIG_XEMU_FEATURE_AUDIO_PACKS
void xemu_audio_packs_apu_prepare_voice_if_needed(struct MCPXAPUState *d, uint16_t voice);
void xemu_audio_packs_apu_override_stream_samples(uint16_t voice,
                                                  float samples[][2],
                                                  int count);
#else
static inline void xemu_audio_packs_apu_prepare_voice_if_needed(struct MCPXAPUState *d, uint16_t voice)
{ (void)d; (void)voice; }
static inline void xemu_audio_packs_apu_override_stream_samples(uint16_t voice,
                                                                float samples[][2],
                                                                int count)
{ (void)voice; (void)samples; (void)count; }
#endif
#endif

#ifndef XEMU_FEATURES_AUDIO_PACKS_APU_H
#define XEMU_FEATURES_AUDIO_PACKS_APU_H
#include "config-host.h"
#include <stdint.h>
struct MCPXAPUState;
#ifdef CONFIG_XEMU_FEATURE_AUDIO_PACKS
void xemu_audio_packs_apu_prepare_voice_if_needed(struct MCPXAPUState *d, uint16_t voice);
#else
static inline void xemu_audio_packs_apu_prepare_voice_if_needed(struct MCPXAPUState *d, uint16_t voice)
{ (void)d; (void)voice; }
#endif
#endif

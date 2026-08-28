// xemu custom fork - Audio Packs frontend boundary
#pragma once
#include "config-host.h"
#ifdef CONFIG_XEMU_FEATURE_AUDIO_PACKS
void FeatureAudioPacksDrawSettings();
#else
static inline void FeatureAudioPacksDrawSettings() {}
#endif

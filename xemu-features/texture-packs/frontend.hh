// xemu custom fork - Texture Packs frontend boundary
#pragma once
#include "config-host.h"
#ifdef CONFIG_XEMU_FEATURE_TEXTURE_PACKS
void FeatureTexturePacksDrawSettings();
void FeatureTexturePacksDrawMiscMenuItem();
void FeatureTexturePacksDrawWindow();
bool FeatureTexturePacksWindowOpen();
void FeatureTexturePacksFrameSync();
void FeatureTexturePacksProcessHotkeys();
void FeatureTexturePacksRenderCommit();
#else
static inline void FeatureTexturePacksDrawSettings() {}
static inline void FeatureTexturePacksDrawMiscMenuItem() {}
static inline void FeatureTexturePacksDrawWindow() {}
static inline bool FeatureTexturePacksWindowOpen() { return false; }
static inline void FeatureTexturePacksFrameSync() {}
static inline void FeatureTexturePacksProcessHotkeys() {}
static inline void FeatureTexturePacksRenderCommit() {}
#endif

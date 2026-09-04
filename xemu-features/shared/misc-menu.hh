// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - feature-owned Misc menu/window aggregation helpers
#pragma once

#include "config-host.h"
#include "ui/xui/common.hh"
#include "xemu-features/texture-packs/frontend.hh"
#include "xemu-features/audio-packs/frontend.hh"
#include "xemu-features/fast-forward/frontend.hh"
#include "xemu-features/freecam/frontend.hh"
#include "xemu-features/geometry-dumper/frontend.hh"
#include "xemu-features/disc-modding/frontend.hh"

static inline void FeatureCustomToolsDrawMiscMenuItems()
{
#if defined(CONFIG_XEMU_FEATURE_TEXTURE_PACKS)
    FeatureTexturePacksDrawMiscMenuItem();
#endif
#if defined(CONFIG_XEMU_FEATURE_AUDIO_PACKS)
    FeatureAudioPacksDrawMiscMenuItem();
#endif
#if defined(CONFIG_XEMU_FEATURE_FAST_FORWARD)
    FeatureFastForwardDrawMiscMenuItem();
#endif
#if defined(CONFIG_XEMU_FEATURE_DISC_MODDING)
    FeatureDiscModdingDrawMiscMenuItem();
#endif

#if defined(CONFIG_XEMU_FEATURE_DEBUG_TOOLS)
#if defined(CONFIG_XEMU_FEATURE_TEXTURE_PACKS) || \
    defined(CONFIG_XEMU_FEATURE_AUDIO_PACKS) || \
    defined(CONFIG_XEMU_FEATURE_FAST_FORWARD) || \
    defined(CONFIG_XEMU_FEATURE_DISC_MODDING)
    ImGui::Separator();
#endif
    FeatureFreecamDrawMiscMenuItem();
    FeatureGeometryDumperDrawMenuItem();
#endif
}

static inline void FeatureCustomToolsShowWindows()
{
    FeatureTexturePacksDrawWindow();
    FeatureAudioPacksDrawWindow();
    FeatureFastForwardDrawWindow();
    FeatureFreecamDrawWindow();
    FeatureGeometryDumperDrawWindow();
    FeatureDiscModdingDrawWindow();
}

static inline bool FeatureCustomToolsWindowsOpen()
{
    return FeatureTexturePacksWindowOpen() ||
           FeatureAudioPacksWindowOpen() ||
           FeatureFastForwardWindowOpen() ||
           FeatureFreecamWindowOpen() ||
           FeatureGeometryDumperWindowOpen() ||
           FeatureDiscModdingWindowOpen();
}

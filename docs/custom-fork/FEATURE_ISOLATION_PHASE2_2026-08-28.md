# Custom Fork Feature Isolation — Phase 2

Date: 2026-08-28

## Objective

Continue the Phase-1 frontend extraction into hardware-facing features without
changing guest-visible behavior. The main goal is reviewability: an upstream
Xemu developer should be able to distinguish the emulator's hardware model from
optional dump/replacement policy at a glance.

## Completed in Phase 2

### 1. Texture pack implementation moved out of NV2A GL

The implementation previously lived at:

`hw/xbox/nv2a/pgraph/gl/texture-io.c/.h`

It now lives under:

- `hw/xbox/features/texture-packs.c/.h`
- `hw/xbox/features/texture-packs-gl.c/.h`

The old NV2A `texture-io.*` paths are compatibility shims only and are no longer
compiled.

### 2. Generic texture-pack core is renderer-neutral

OpenGL upload calls were split into `texture-packs-gl.c`. The generic pack core
contains indexing, paths, image/animation decode, dumps and cache state, but no
`glTexImage*`, `glGenerateMipmap`, `GLenum`, or `<epoxy/gl.h>` dependency.

This makes the ownership boundary explicit:

```text
texture-packs.c          texture-packs-gl.c
index/decode/cache  ---> prepared pixels ---> GL upload
       |                                      |
       +-------------------------------> Vulkan consumes generic pixels
```

### 3. NV2A no longer reads custom texture settings directly

`pgraph/gl/texture.c` and `pgraph/vk/texture.c` no longer reference
`g_config.general.texture_*`.

They ask the feature boundary instead:

- dump enabled?
- replacement enabled?
- should this mip level be dumped?
- are dynamic replacements active?

That keeps the renderer independent of the custom settings schema.

### 4. Renderer cache invalidation reduced to one feature hook

Each backend now exposes one renderer-thread synchronization point:

```c
xemu_texture_packs_renderer_sync(pgraph_*_texture_cache_flush);
```

The feature owns request consumption; the renderer only provides the backend
operation that is safe on that thread.

### 5. Audio pack implementation moved out of MCPX APU

The implementation previously lived at:

`hw/xbox/mcpx/apu/audio-io.c/.h`

It now lives under:

- `hw/xbox/features/audio-packs.c/.h`
- `hw/xbox/features/audio-packs-apu.c/.h`

The old APU `audio-io.*` paths are compatibility shims and are not compiled.

### 6. Static voice discovery/snapshot removed from `vp.c`

The feature-specific static source snapshot code was about 220 lines inside the
Voice Processor. It decoded PCM/Xbox ADPCM solely so dumping/replacement could
identify an asset.

That code now lives in `audio-packs-apu.c`. `vp.c` has one explicit discovery
hook:

```c
xemu_audio_packs_apu_prepare_voice_if_needed(d, v);
```

The adapter intentionally duplicates two tiny read helpers rather than making
private VP implementation functions public solely for a custom feature.

As a result, `vp.c` decreased from 2,154 to 1,932 lines in this source lineage.

### 7. MCPX VP no longer knows audio-pack enable settings

`vp.c` no longer reads:

- `g_config.audio.dump_enabled`
- `g_config.audio.replace_enabled`
- `g_config.audio.dump_skip_replaced`

The feature decides whether source discovery is useful. In particular,
replacement-only mode with an empty replacement index remains a true no-op,
but that policy no longer leaks into MCPX code.

## Current bridge surface

The automated isolation audit reports only explicitly allowed hardware/UI
bridge files. At Phase-2 completion:

- texture packs: GL texture backend + Vulkan texture backend + generic UI hook
  sites;
- audio packs: APU lifecycle + VP event/sample hook sites + generic UI hook;
- no unexpected feature-leak files;
- no direct custom texture settings in NV2A texture backends;
- no direct audio-pack enable settings in MCPX VP.

Run:

```bash
./scripts/xemu-feature-isolation-smoke.py
./scripts/xemu-feature-isolation-phase2-smoke.py
./scripts/xemu-feature-isolation-audit.py
```

## Compatibility policy

This phase is ownership-only. It intentionally preserves:

- texture dump color normalization;
- GL/Vulkan replacements;
- GIF/WebP animation;
- procedural texture shaders;
- texture dump/replacement hot reload;
- audio dumping/replacement;
- preloaded replacement WAV cache;
- random `_1.wav`, `_2.wav`, ... variants;
- automatic guest-CBO retrigger/seek handling;
- shorter/longer replacement audio;
- optional stronger JSON monophonic retrigger override.

## Next phase

The largest remaining texture-pack leakage is custom replacement/animation/
procedural-shader state embedded directly in GL/Vulkan `TextureBinding`
structures and renderer-local helper code.

Phase 3 should move that state to feature-owned renderer sidecars/adapters so
core renderer structures approach upstream shape again. After that, add
independent Meson build switches so each feature can be compiled out rather
than merely disabled at runtime.

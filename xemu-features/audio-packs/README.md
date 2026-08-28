# Audio Packs

## Purpose

Source-level MCPX/APU audio dumping and WAV replacement before guest voice processing. It preserves guest-controlled pitch, volume, envelopes, filters, looping/retrigger semantics and supports preloaded randomized replacement variants.

## Build gate

- Meson: `xemu_feature_audio_packs`
- Config macro: `CONFIG_XEMU_FEATURE_AUDIO_PACKS`
- Default in this custom fork: ON

## Public API

`audio-packs.h` owns lifecycle, path/index, replacement/dump and per-voice state APIs. `audio-packs-apu.h` is the narrow APU preparation bridge. `frontend.hh` owns Settings UI.

## Files owned

- `audio-packs-apu.c`
- `audio-packs-apu.h`
- `audio-packs.c`
- `audio-packs.h`
- `frontend.cc`
- `frontend.hh`

## Exact Xemu hook sites

- `hw/xbox/mcpx/apu/apu.c` — init/reset/finalize lifecycle.
- `hw/xbox/mcpx/apu/vp/vp.c` — voice reset/reuse, guest CBO writes, replacement sample/rate/end queries, frame-boundary synchronization.
- `ui/xui/main-menu.cc` — one settings-panel hook.
- `hw/xbox/mcpx/apu/meson.build` and `ui/xui/meson.build` — conditional source inclusion.

## Dependencies

Uses existing MCPX/APU data structures, SDL WAV/audio conversion, GLib containers/threads and Xemu settings. It has no required dependency on the other optional features.

## Threading model

Replacement WAV preload uses a GLib thread pool (up to four workers). Dumping uses one background `xemu.audio-dump` worker. APU voice workers consume predecoded/indexed data; index publication and cache mutation are synchronized and deferred to safe boundaries.

## Hot-path behavior

Disabled public hooks are inline false/no-op. When built but inactive, dump/replacement gates early-out. Voice workers do not open files or decode WAVs. The replacement index/preloaded asset set is prepared before publication.

## Build-disabled behavior

No audio-pack implementation objects are linked. MCPX voice hooks compile to neutral defaults; ordinary Xbox audio processing is unchanged.

## Porting only this feature

Copy `xemu-features/audio-packs/`, add the Meson option/config-host flag, conditionally compile its frontend/APU sources, and apply only the four integration sites above.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.

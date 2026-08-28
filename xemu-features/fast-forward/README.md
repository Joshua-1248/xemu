# Fast Forward

## Purpose

Unlimited and 2×–5× host pacing, VBLANK/render throttling, guest-clock compensation, hotkey/UI state and optional preserve-pitch host-audio transformation.

## Build gate

- Meson: `xemu_feature_fast_forward`
- Config macro: `CONFIG_XEMU_FEATURE_FAST_FORWARD`
- Default in this custom fork: ON

## Public API

`fast-forward.h` is the global mode/query API. `timing.h` owns VBLANK/timer/thread/guest-clock policy. `audio.h` owns completed-host-block transformation. `frontend.hh` owns settings/hotkey UI.

## Files owned

- `audio.c`
- `audio.h`
- `fast-forward.h`
- `frontend.cc`
- `frontend.hh`
- `timing.c`
- `timing.h`

## Exact Xemu hook sites

- `ui/xemu.c` — four timing/presentation-policy calls.
- `util/main-loop.c` — main-loop unblock query (weak false fallback only while the feature is built, for standalone QEMU utility binaries).
- `hw/xbox/mcpx/apu/monitor.c` — one completed-block audio transform/reset boundary.
- `hw/xbox/mcpx/apu/apu.c` — APU pacing query.
- `ui/xui/main.cc` / `ui/xui/main-menu.cc` — hotkey and settings hooks.
- `ui/xui/meson.build` and `hw/xbox/mcpx/apu/meson.build` — conditional source inclusion.

## Dependencies

Uses SDL host audio and Xemu timer/clock APIs. TAS interaction is passed as simple `tas_stepping` facts or through neutral public APIs; Fast Forward does not require TAS to be built.

## Threading model

Mode state is atomic. Timing policy runs at existing VBLANK/main-loop boundaries. Preserve-pitch state is owned by the audio transform and does not add a worker thread.

## Hot-path behavior

When compiled out, timing functions return normal intervals/render=true and audio submission returns false so ordinary core paths execute directly. When inactive at runtime, mode queries return normal and audio resets to frequency ratio 1.0.

## Build-disabled behavior

Frontend/timing/audio objects are omitted. Core timing/audio calls inline to ordinary behavior, and the `util/main-loop.c` weak fallback symbol is not emitted.

## Porting only this feature

Copy `xemu-features/fast-forward/`, add the four timing hooks, one main-loop query, one APU audio transform and the UI/Meson gates.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.

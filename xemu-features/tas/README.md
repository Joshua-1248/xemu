# TAS / TAStudio

## Purpose

Deterministic input recording/playback, frame advance, lag tracking, input automation and the TAStudio frontend/editor.

## Build gate

- Meson: `xemu_feature_tas`
- Config macro: `CONFIG_XEMU_FEATURE_TAS`
- Default in this custom fork: ON

## Public API

`tas.h` is the C core boundary for VBLANK/XID/input state. `studio.hh` is the frontend boundary for menu/windows/settings/snapshot notifications.

## Files owned

- `core.c`
- `studio.cc`
- `studio.hh`
- `tas.h`

## Exact Xemu hook sites

- `hw/xbox/xid.c` — process/override controller reports.
- `ui/xemu.c` — VBLANK, frame-advance and pause-request hooks.
- `ui/xui/main.cc`, `ui/xui/main-menu.cc`, `ui/xui/menubar.cc` — windows/menu/settings integration.
- `hw/xbox/meson.build`, `ui/xui/meson.build`, root `meson.build` — conditional core/UI/shared-memory source inclusion.

## Dependencies

Uses shared guest-memory, snapshot/UI/file helpers, and may call the Fast Forward public API. That API becomes neutral when Fast Forward is compiled out, so TAS remains buildable independently.

## Threading model

Core input state uses atomics for read-mostly/hot XID paths and mutexes for recording/playback buffers. No dedicated TAS OS worker is created.

## Hot-path behavior

The XID hook is guarded by `G_UNLIKELY(xemu_tas_enabled())`; automation uses packed atomic masks/words to avoid taking a mutex on ordinary controller polling.

## Build-disabled behavior

Core/UI TAS objects are omitted. XID/VBLANK/menu calls reduce to neutral inline functions; no TAS state, polling or input rewriting remains.

## Porting only this feature

Copy `xemu-features/tas/` plus shared guest-memory, add XID/VBLANK/UI hook calls and the option/config flag. Fast Forward is optional rather than a hard dependency.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.

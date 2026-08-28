# 0–200% Volume Amplifier

## Purpose

Optional host-output extension that preserves Xemu’s perceptual 0–100% gain curve and extends output to a direct 2.0× gain at 200%.

## Build gate

- Meson: `xemu_feature_volume_amplifier`
- Config macro: `CONFIG_XEMU_FEATURE_VOLUME_AMPLIFIER`
- Default in this custom fork: ON

## Public API

`volume.h` exposes `xemu_volume_amplifier_apply/reset()` plus `xemu_volume_amplifier_max()`. The implementation owns gain-setter caching.

## Files owned

- `volume.c`
- `volume.h`

## Exact Xemu hook sites

- `hw/xbox/mcpx/apu/monitor.c` — one gain-application/reset boundary, with the native 0–100% fallback retained in core.
- `ui/xui/main-menu.cc` and `ui/xui/popup-menu.cc` — slider maximum query.
- `hw/xbox/mcpx/apu/meson.build` — conditional source inclusion.

## Dependencies

Uses SDL audio-stream gain and the existing `g_config.audio.volume_limit` setting. It has no dependency on Fast Forward or Audio Packs.

## Threading model

No worker thread. State is limited to the current SDL stream and cached source volume/applied gain.

## Hot-path behavior

`pow()` is evaluated only when the requested volume changes. Repeated monitor frames with the same volume avoid redundant gain setter calls.

## Build-disabled behavior

The amplifier object is omitted. `xemu_volume_amplifier_max()` becomes 1.0, `apply()` returns false, and `monitor.c` uses the ordinary 0–100% Xemu gain path.

## Porting only this feature

Copy `xemu-features/volume-amplifier/`, add the monitor hook and UI max query, and conditionally compile `volume.c`.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.

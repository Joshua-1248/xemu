# 0–300% Volume Amplifier + Mute Hotkey

## Purpose

Optional host-output extension that preserves Xemu's perceptual 0–100% gain
curve and extends output to a direct 3.0× gain at 300%.

The feature also provides a simple global mute toggle:

```text
M
```

The mute state is runtime-only. Muting does not overwrite
`g_config.audio.volume_limit`; unmuting immediately restores the currently
configured output level.

## Build gate

- Meson: `xemu_feature_volume_amplifier`
- Config macro: `CONFIG_XEMU_FEATURE_VOLUME_AMPLIFIER`
- Default in this custom fork: ON

## Public API

`volume.h` exposes:

- `xemu_volume_amplifier_apply/reset()`
- `xemu_volume_amplifier_max()`
- `xemu_volume_amplifier_is_muted()`
- `xemu_volume_amplifier_toggle_mute()`

The implementation owns gain-setter caching and mute-state synchronization.

## Files owned

- `volume.c`
- `volume.h`

## Exact Xemu hook sites

- `hw/xbox/mcpx/apu/monitor.c` — one gain-application/reset boundary, with the
  native 0–100% fallback retained in core.
- `ui/xui/main-menu.cc` and `ui/xui/popup-menu.cc` — slider maximum query.
- `hw/xbox/mcpx/apu/meson.build` — conditional source inclusion.

No additional native hotkey hook is required. The feature registers one SDL
event watch when the audio boundary first becomes active.

## Mute hotkey behavior

A non-repeated `SDL_EVENT_KEY_DOWN` for the `M` key atomically toggles mute.

The SDL event-watch callback does **not** call into the audio stream. It only
changes feature-owned atomic state. The existing audio monitor path observes the
new mute revision and applies either zero gain or the user's current configured
gain on its next normal update.

This keeps the event callback lightweight and safe even if SDL invokes an event
watch from a different thread.

## Dependencies

Uses SDL audio-stream gain, SDL event watching, and the existing
`g_config.audio.volume_limit` setting. It has no dependency on Fast Forward or
Audio Packs.

## Threading model

No worker thread.

- SDL event-watch side: atomic mute state/revision only.
- Audio side: current stream, source-volume cache, applied-gain cache, and last
  consumed mute revision.

## Hot-path behavior

`pow()` is evaluated only when the requested volume or mute revision changes.
Repeated monitor frames with the same volume/mute state avoid redundant gain
setter calls.

## Build-disabled behavior

The amplifier object is omitted. `xemu_volume_amplifier_max()` becomes 1.0,
mute APIs are neutral, `apply()` returns false, and `monitor.c` uses the ordinary
0–100% Xemu gain path.

## Porting only this feature

Copy `xemu-features/volume-amplifier/`, add the monitor hook and UI max query,
and conditionally compile `volume.c`.

The neutral public-header contract should be retained when porting so unrelated
core code does not need `#ifdef` forests.

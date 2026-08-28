# Custom Fork Feature Isolation Architecture

## Purpose

The custom fork keeps feature development separate from ordinary Xbox emulation.
The architectural target is:

```text
                  upstream-shaped Xemu / QEMU core
                              |
                    tiny explicit hook/event
                              |
          +-------------------+-------------------+
          |                   |                   |
     xemu-features/       xemu-features/      xemu-features/
       tas/                 audio-packs/        texture-packs/
     owns state/UI         owns cache/I/O       owns sidecars/I/O
```

A feature is a module/subsystem, not automatically an OS thread. Threads are
used only for genuinely blocking/asynchronous work such as audio preload/dump
I/O.

## Phase 4 rules

1. **Feature-owned state stays in `xemu-features/`.** Generic UI/hardware files
   must not own feature caches, editor state, random state or transform state.
2. **Core hooks stay narrow.** Hardware/frontend hook sites pass only the facts
   needed at the boundary and receive a simple result/decision.
3. **Build-disabled means physically absent.** Each user-facing feature has an
   independent Meson boolean option. OFF omits implementation objects.
4. **Public APIs are compile-time neutral.** Feature headers use
   `CONFIG_XEMU_FEATURE_*` and provide inline false/no-op/default behavior when
   their implementation is omitted.
5. **No blocking I/O on emulation workers.** APU/NV2A/CPU hot paths may consume
   prepared data but must not open/decode/scan files.
6. **Cross-feature dependencies use public boundaries only.** For example, TAS
   can include the Fast Forward public API; if Fast Forward is OFF that API is
   neutral rather than creating a hard link dependency.
7. **Native Xemu ownership stays native.** SnapshotManager,
   NotificationManager, ordinary Monitor/Audio/Video debug UI, XBE handling and
   `fast_hash` are not moved merely because custom features interact with them.
8. **Original core structures should not grow for optional features.** Texture
   replacement/animation/shader state is held in feature sidecars, not renderer
   `TextureBinding`. Fast Forward/volume transform state is not stored in
   `MCPXAPUState`.
9. **One feature must be portable independently.** Each feature directory
   contains a README and export manifest with exact integration sites.
10. **Behavior preservation precedes semantic changes.** Isolation should not
    alter confirmed Fast Forward, texture, audio, cheat, TAS, scripting or
    debugger behavior.

## Current top-level ownership

```text
xemu-features/
  audio-packs/
  texture-packs/
  cheats/
  tas/
  scripting/
  debug-tools/
  fast-forward/
  volume-amplifier/
  shared/
```

`shared/guest-memory.*` is not a user-facing feature. It is linked only when at
least one optional tool that needs guest-memory access is enabled.

## Independent build switches

- `xemu_feature_audio_packs`
- `xemu_feature_texture_packs`
- `xemu_feature_cheats`
- `xemu_feature_tas`
- `xemu_feature_scripting`
- `xemu_feature_debug_tools`
- `xemu_feature_fast_forward`
- `xemu_feature_volume_amplifier`

All default ON in the custom fork. Compatibility/debugging builds may disable
any subset.

## Hot-path examples

### Audio Packs

MCPX voice workers consume preloaded replacement WAV/index state. WAV file I/O
and decoding do not occur on voice workers. Cache invalidation is synchronized
at safe APU boundaries.

### Texture Packs

NV2A GL/Vulkan paths call lifecycle/query hooks. Long-lived custom state lives
in feature-owned sidecars keyed by the ordinary renderer binding pointer.
Ordinary bindings get no sidecar if no custom state is needed.

### Fast Forward

VBLANK/timer/thread/guest-clock policy lives in the Fast Forward timing module.
MCPX monitor offers a completed host block to the optional FF audio transform.
With FF OFF, timing calls return normal behavior and the audio hook returns
false so the ordinary monitor path submits the block.

### Volume Amplifier

The optional module owns the 0–200% gain extension/cache. With it OFF, the
slider maximum is 100% and `monitor.c` retains the normal 0–100% Xemu gain path.

## Validation

Run:

```bash
python3 scripts/xemu-feature-isolation-audit.py
```

The Phase 4 audit checks ownership, Meson/config switches, stale-path removal,
core config leakage, renderer/APU state invariants, conditional helpers, and
public-header syntax for all-OFF, all-ON and every single-feature mode.

A real `qemu-system-i386` compile/link remains the final validation because
header/static audits cannot prove the complete dependency graph.

# Feature Isolation Phase 4 — top-level ownership and physical compile-out

## Baseline

Phase 4 was developed directly against the user's current `Main(5).zip`. That tree already contained the complete Phase 1/2/3 isolation content. Phase 4 therefore changes ownership/build boundaries without reconstructing older overlays.

## Result

All major fork-specific implementations now live under top-level `xemu-features/`:

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

Eight independent Meson switches control physical source inclusion. Public feature headers use matching `CONFIG_XEMU_FEATURE_*` macros and provide neutral inline behavior when omitted.

## Fast Forward and volume extraction

`hw/xbox/mcpx/apu/monitor.c` no longer owns preserve-pitch block selection, crossfade state, audio-speed frequency-ratio caching, or the custom 200% gain cache. It produces the ordinary completed host block and offers it to two narrow optional transforms:

1. `xemu_volume_amplifier_apply()` — if not consumed, monitor retains the native 0–100% gain path.
2. `xemu_fast_forward_audio_submit()` — if not consumed, monitor submits the ordinary completed block.

The corresponding custom state was removed from `MCPXAPUState`. Fast Forward VBLANK/render/timer/thread/guest-clock policy is likewise owned by `xemu-features/fast-forward/timing.c` rather than `ui/xemu.c`.

## Renderer state

Phase 3's sidecar invariant is retained. OpenGL and Vulkan `TextureBinding` structures carry no custom replacement/animation/shader fields. Generic texture-pack code is compiled once from the renderer-neutral pgraph layer; GL/Vulkan add only their adapters.

## Native ownership preserved

The refactor intentionally leaves these native Xemu facilities in their normal locations: SnapshotManager, NotificationManager, ordinary Monitor/Audio/Video debug UI, XBE handling and `fast_hash`. Only fork-specific extensions are under `xemu-features/`.

## Shared helper ownership

The old root-level `xemu-guestmem.*` and `xemu-dbg.*` custom helpers were removed. Guest-memory tooling is `xemu-features/shared/guest-memory.*` and links only if a dependent tool is built. The custom debugger API is under `xemu-features/debug-tools/` and links only with Debug Tools.

## Validation

`scripts/xemu-feature-isolation-audit.py` validates physical ownership, eight Meson/config switches, stale-path removal, absence of texture/audio config leakage in NV2A/MCPX core, Fast Forward/volume extraction, renderer sidecars, conditional shared helpers, and public-header syntax with all features OFF/ON and representative single-feature configurations.

A full Xemu compile/link is still required on the user's Linux tree. The current analysis container cannot finish a fresh Meson configure because the host development environment lacks the `glib-2.0` dependency.

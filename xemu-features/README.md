# Xemu Custom Fork Feature Layer

The Phase-4 isolation architecture remains the organizing rule for the current
feature suite.

This tree owns the fork-specific feature implementations. Ordinary Xemu/QEMU and Xbox hardware files keep only small integration hooks. The goal is that any feature can be compiled out physically, reviewed independently, or ported without dragging unrelated custom systems along.

## Build options

All options default to `true` for the custom fork:

| Feature | Meson option | Config macro |
|---|---|---|
| Audio Packs | `xemu_feature_audio_packs` | `CONFIG_XEMU_FEATURE_AUDIO_PACKS` |
| Texture Packs | `xemu_feature_texture_packs` | `CONFIG_XEMU_FEATURE_TEXTURE_PACKS` |
| Cheats / Patches | `xemu_feature_cheats` | `CONFIG_XEMU_FEATURE_CHEATS` |
| TAS / TAStudio | `xemu_feature_tas` | `CONFIG_XEMU_FEATURE_TAS` |
| Lua / Python Scripting | `xemu_feature_scripting` | `CONFIG_XEMU_FEATURE_SCRIPTING` |
| Debug Tools | `xemu_feature_debug_tools` | `CONFIG_XEMU_FEATURE_DEBUG_TOOLS` |
| Fast Forward | `xemu_feature_fast_forward` | `CONFIG_XEMU_FEATURE_FAST_FORWARD` |
| Disc Files & Mods | `xemu_feature_disc_modding` | `CONFIG_XEMU_FEATURE_DISC_MODDING` |
| 0–200% Volume Amplifier | `xemu_feature_volume_amplifier` | `CONFIG_XEMU_FEATURE_VOLUME_AMPLIFIER` |

Example all-off configure arguments:

```text
-Dxemu_feature_audio_packs=false
-Dxemu_feature_texture_packs=false
-Dxemu_feature_cheats=false
-Dxemu_feature_tas=false
-Dxemu_feature_scripting=false
-Dxemu_feature_debug_tools=false
-Dxemu_feature_fast_forward=false
-Dxemu_feature_disc_modding=false
-Dxemu_feature_volume_amplifier=false
```

## Ownership rules

- Feature state, caches, filesystem work, editors and feature-specific workers live here.
- Native Xemu subsystems stay native. SnapshotManager, NotificationManager, ordinary Monitor/Audio/Video debug UI, XBE handling and `fast_hash` are not reclassified as custom features.
- Hardware/frontend hook sites pass only the facts needed at the boundary.
- Public headers include `config-host.h` and provide neutral inline behavior when their feature is omitted.
- Build-disabled feature implementation sources are not added to source sets.
- Shared guest-memory support is under `shared/` and is linked only when at least one tool that needs it is built.
- Feature-owned bundled third-party source is isolated under `dependencies/`; it keeps its own upstream license and is not treated as fork-authored code. Capstone 5.0.9 is currently bundled there for x86 disassembly.

See the feature-directory `README.md` files and any accompanying
`EXPORT_MANIFEST.txt` files for their integration surfaces.

## Custom UI organization

The primary runtime/modding tools are grouped under `Misc` rather than being
scattered through native Settings pages. With the corresponding features
enabled, the main block is ordered:

```text
Cheats/Patches
Texture Packs
Audio Packs
Fast Forward
Disc Files & Mods
---
Free Camera
Geometry Dumper
```

Lua and Python consoles follow below that block. Feature windows that opt into
`shared/detachable-windows.hh` can be torn into native OS windows; title-bar
drag tear-off transfers the active drag directly to the native host instead of
requiring a release/re-grab cycle.

The custom Debugger/Disassembler also includes an in-process **Memory Search**
tab ported from the standalone Xemu Cheat Engine workflow. It supports typed
first/next scans, Unknown Value Search, changed/increased/decreased narrowing,
physical/virtual regions, strings/AOB patterns, paged live results, and direct
handoff to the Memory viewer, Disassembler, Globals, and watchpoints.


## Development feature: Free Camera

`freecam/` contains the renderer-level free-camera work. Milestone 5 remains
feature-owned and is wired through the existing Texture Packs/Geometry Dumper
renderer shim. Its menu item is exposed by the existing `Misc` feature
aggregator and F10 toggles the camera. Projective compatibility remains intact.
Reconstructed View now first factors a camera-like pre-projection transform
directly from perspective `CMAT`, falls back to the Milestone 4 `MMAT0 + CMAT`
split when useful, and only then uses the old projective compatibility path for
remaining 3D draws. Obvious 2D/non-perspective fixed-function draws are left
unchanged in Reconstructed View. Programmable VSH draws retain the validated
post-VSH compatibility tail. No native/upstream Xemu/QEMU source is owned by
the feature.

# Phase 4 build matrix

The intended compatibility/debugging matrix is:

- all eight features ON (custom-fork default)
- all eight features OFF (pure core plus native Xemu facilities)
- Audio Packs only
- Texture Packs only
- Cheats only
- TAS only
- Scripting only
- Debug Tools only
- Fast Forward only
- Volume Amplifier only
- arbitrary combinations

## Meson switches

```text
xemu_feature_audio_packs
xemu_feature_texture_packs
xemu_feature_cheats
xemu_feature_tas
xemu_feature_scripting
xemu_feature_debug_tools
xemu_feature_fast_forward
xemu_feature_volume_amplifier
```

A switch set to `false` omits that feature's implementation translation units. Core/frontend hook headers then resolve to inline neutral behavior.

## Validation commands

Structural/interface audit:

```bash
python3 scripts/xemu-feature-isolation-audit.py
```

Authoritative user compile/link test for the default all-ON tree:

```bash
ninja -C build qemu-system-i386
```

For build-matrix testing, reconfigure separate build directories with the desired `-Dxemu_feature_*=false/true` values and build `qemu-system-i386`.

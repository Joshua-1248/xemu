# Geometry Dumper

Feature-owned NV2A raw geometry capture for the xemu custom fork.

## Current capture modes

- **Capture Next Draw**: captures the next actual PGRAPH `flush_draw` batch.
- **Capture Next Frame**: arms after the next completed flip and captures every
  draw until the following flip, ensuring a full subsequent frame rather than a
  partial current frame.

## Output

Each capture directory contains:

- `geometry.obj` — combined OBJ with one object/group per NV2A draw/segment.
- `draws.jsonl` — one metadata record per captured draw, including primitive
  mode, segment boundaries, vertex source, DMA handles, all 16 NV2A vertex
  attribute descriptors, and all four texture-stage register snapshots.
- `vertices.csv` — decoded per-occurrence position/normal/UV0/diffuse values
  plus the original Xbox source vertex/index so indexed-strip degenerates and
  source-buffer relationships remain recoverable.

The OBJ is deliberately **raw/pre-vertex-shader**. It does not flip UVs, swap
axes, generate normals, weld vertices, smooth meshes, or otherwise reinterpret
the source data. Quads/quad strips are triangulated with the same split used by
xemu's NV2A geometry path, and indexed strip degenerates are detected from the
original Xbox source indices. This keeps the dump useful as reverse-engineering
evidence.

## Integration

To preserve the fork's hard rule against modifying upstream Xemu/QEMU source for
custom features, `geometry-dumper.c` is compiled through the existing
feature-owned texture-pack PGRAPH translation unit, and the UI is compiled through the existing feature-owned debug-tools frontend
translation unit. Display/menu dispatch is handled by the feature-owned shared
`Misc` aggregator rather than the native Debug menu.

At renderer initialization, the geometry feature copies the active upstream
`PGRAPHRenderer` descriptor and wraps only `flush_draw` and `flip_stall` in the
feature-owned copy. The original callbacks are still invoked unchanged.

## UI organization

Geometry Dumper is exposed under `Misc`, directly below Free Camera. The main
window separates capture controls/status from an `Info` tab containing the
long-form glTF/export and serialization notes. The window also participates in
the shared detachable-window system.

# Texture Packs

## Purpose

Texture dumping/replacement for NV2A with GL and Vulkan adapters, animated GIF/WebP replacements, procedural shader replacements, dump color conversion and feature-owned renderer sidecars.

## Build gate

- Meson: `xemu_feature_texture_packs`
- Config macro: `CONFIG_XEMU_FEATURE_TEXTURE_PACKS`
- Default in this custom fork: ON

## Public API

`texture-packs.h` is renderer-neutral. `texture-packs-gl.h` and `texture-packs-vk.h` expose backend lifecycle/upload/dump/sidecar hooks. `frontend.hh` owns settings, hotkeys and render/frame synchronization.

## Files owned

- `frontend.cc`
- `frontend.hh`
- `texture-packs-gl.c`
- `texture-packs-gl.h`
- `texture-packs-vk.c`
- `texture-packs-vk.h`
- `texture-packs.c`
- `texture-packs.h`

## Exact Xemu hook sites

- `hw/xbox/nv2a/pgraph/gl/texture.c` — binding lifecycle, sampler override, replacement upload, dumps, dynamic refresh and backend sync.
- `hw/xbox/nv2a/pgraph/vk/texture.c` — creation plan, binding lifecycle, replacement upload, dumps, dynamic refresh and backend sync.
- `ui/xui/main.cc` / `ui/xui/main-menu.cc` — frame/hotkey/render-commit and settings hooks.
- `hw/xbox/nv2a/pgraph/{meson.build,gl/meson.build,vk/meson.build}` and `ui/xui/meson.build` — conditional source inclusion.

## Dependencies

Uses existing NV2A texture metadata and host GL/Vulkan APIs plus Xemu settings/file helpers. The renderer-neutral implementation is compiled once from `pgraph/meson.build`; GL and Vulkan compile only their adapters.

## Threading model

No feature-specific permanent worker thread is introduced. Filesystem/index work is feature-owned; renderer hot paths consume prepared lookup state. GL/Vulkan sidecars are keyed by ordinary `TextureBinding *` identity.

## Hot-path behavior

With the feature off, every renderer hook is an inline neutral operation and no sidecar is allocated. With it on, ordinary textures that need no custom state do not receive sidecars.

## Build-disabled behavior

No texture-pack implementation or backend adapter objects are linked. Core `TextureBinding` structures remain upstream-shaped and dump/replace hooks compile away.

## Porting only this feature

Copy `xemu-features/texture-packs/`, add the option/config flag, compile the generic source once plus the selected renderer adapter(s), then add the documented texture lifecycle/query hooks.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.

## Experimental material-map sidecars

A procedural `<hash>.shader` can sample optional image sidecars that share the
base replacement hash:

- `<hash>_n.png`  -> `iNormalMap` / `iHasNormalMap`
- `<hash>_s.png`  -> `iSpecularMap` / `iHasSpecularMap`
- `<hash>_d.png`  -> `iDisplacementMap` / `iHasDisplacementMap`
- `<hash>_ao.png` -> `iAOMap` / `iHasAOMap`

The same stem rules and supported replacement image formats apply. Sidecars are
bound on both the OpenGL and Vulkan shader-replacement paths. Missing maps get
safe 1x1 defaults so a shader may sample the samplers unconditionally and use
the `iHas*` flags to decide whether the authored map is present.

These are texture-shader inputs, not yet a replacement for the game's actual
NV2A draw-time lighting/material pipeline. In particular, a normal/specular
map used here produces whatever texture-space effect the `.shader` authors;
it does not automatically react to the game's real lights or camera. Likewise
`_d` is height data available to the shader, not automatic geometry
subdivision/displacement. A draw-time material enhancement layer is the proper
future path for physically meaningful normal/specular/displacement behavior.

See `examples/material-maps.shader` for a deliberately simple texture-space
preview shader.


## Experimental built-in material-map enhancement

In addition to optional per-texture `.shader` files, the texture-pack feature can now apply a built-in enhancement pass to ordinary replacement textures. When enabled from the Texture Packs settings panel, a replacement texture that has one or more sidecars like `_n`, `_s`, `_d`, or `_ao` will automatically get a game-agnostic lighting pass without requiring a custom `.shader` file or any game-specific lighting reverse engineering.

Current behavior:
- `_n` = tangent-space normal map
- `_s` = grayscale specular mask
- `_d` = grayscale height map for built-in view-dependent parallax/relief mapping (white = raised, black = recessed)
- `_ao` = grayscale ambient occlusion map
- Light modes: headlight or user-configurable directional light
- Tunable controls: normal strength, ambient, diffuse, specular, gloss, parallax, AO strength, and normal Y flip

This is intentionally an enhancement layer provided by Xemu itself rather than an attempt to reproduce each game's original lighting model.

### Camera-reactive headlight (V3)

The built-in material enhancer can derive a camera-relative tangent-space light direction from each NV2A draw without using game-specific lighting semantics. The existing geometry-dumper renderer wrapper observes draw input and post-vertex-shader texture coordinates:

- fixed-function draws reconstruct a view-space surface TBN and project a camera-fixed `+Z` headlight axis into that basis;
- programmable vertex-shader draws use post-VSH `(clip.x, clip.y, clip.w)` as a game-independent linear pseudo-view fallback;
- a representative UV tangent/bitangent basis is reconstructed from the draw geometry;
- the resulting tangent-space light direction is fed to the `_n` material prepass and refreshed as the camera/geometry orientation changes.

This keeps the enhancement game-agnostic. It is still a per-texture enhancement pass rather than a native per-fragment replacement of the guest pixel shader, so one representative surface orientation is used for a texture at a time.

### Camera-reactive material stabilization (V4)

The V4 stabilization pass removes several sources of camera-light popping and
slow response introduced by the first V3 prototype:

- camera-light state is keyed by the actual replacement texture content hash,
  not transient NV2A texture stages;
- geometry observation rejects tiny/degenerate triangles and area-weights up
  to 256 representative triangles instead of taking the first valid triangle;
- triangle-strip winding is normalized before tangent-space accumulation;
- per-hash light directions receive light temporal filtering and a small
  angular-noise threshold;
- GL and Vulkan consume the same hash-keyed camera-light state;
- built-in GL material sidecars correctly instantiate without requiring a
  legacy `.shader` file;
- material relighting is dirty-driven and bounded to roughly 60 Hz per
  material instead of being forced from every draw;
- Vulkan relighting is recorded into the normal renderer command buffer with
  explicit render-pass read/write dependencies, eliminating the previous
  per-relight `vkQueueWaitIdle()` synchronization stall;
- replacement-index reload clears camera-light state on the renderer thread;
- material UI configuration reads/writes are synchronized between the UI and
  renderer threads.

The system remains intentionally game-agnostic. Programmable vertex-shader
positions still use the generic post-VSH camera-facing fallback where game
matrix semantics are unavailable.

### Camera-reactive material stabilization (V5)

The V5 audit tightens the remaining hot-path and multi-draw behavior discovered
after V4 runtime testing:

- material-sidecar membership is now a hash-only lookup built with the
  replacement index; camera tracking no longer opens/probes `_n/_s/_d/_ao`
  image files from the draw path;
- the same material hash is accumulated across every draw that uses it in a
  guest frame, weighted by represented geometric area, and the complete-frame
  direction is published for the next frame instead of allowing several draws
  to fight over one shared preprocessed replacement texture;
- a material that disappears for one or more frames discards its pending old
  aggregate and bootstraps from the new draw immediately when it reappears;
- camera-light changes are no longer subjected to the extra ~16 ms shader
  throttle; the frame-wide publication already bounds them to one update per
  material per guest frame, while time-driven animated shader sources retain
  their rate limit;
- repeated NV2A texture dirty notifications for the same resource can trigger
  at most one full guest-texture hash per stage per guest frame instead of one
  hash per draw; hashes that currently own material sidecars are revalidated
  once per frame, while ordinary cached resources get a low-frequency safety
  recheck for in-place guest-VRAM changes;
- OpenGL material rendering now restores the exact previous 2D texture binding
  for texture units 0 through 4 rather than force-binding the enhanced texture
  onto whichever unit happened to be active; animated `iChannel0` refresh is
  also performed only after that state snapshot and restores the caller's
  pixel-unpack row-length/alignment settings.

These changes keep the existing architectural ceiling: a single replacement
texture hash still has one enhanced image at a time. True simultaneously
different lighting for the same hash on differently oriented polygons requires
normal mapping in the actual geometry fragment draw rather than this shared
material prepass.

### Camera-reactive draw synchronization (V6)

Runtime testing after V5 showed that complete-frame hash aggregation could still
produce large lighting swings when one replacement hash was reused by several
differently oriented surfaces. The aggregate itself was mathematically stable,
but it represented incompatible tangent-space light directions with one shared
answer; as the camera moved and the represented area of those surfaces changed,
the normalized average could move sharply.

V6 changes camera-reactive materials to draw-synchronous publication:

- the geometry observer still area-averages valid triangles/segments within the
  current NV2A draw, but publishes that draw result immediately rather than
  accumulating the same hash across the whole guest frame;
- OpenGL registers a feature-owned END-time callback because GL binds guest
  textures at NV097 BEGIN, before the draw's complete vertex data exists. The
  current stage's enhanced binding is refreshed after the TBN is known and
  before `pgraph_gl_flush_draw()` consumes it;
- the GL material pass now isolates additional mid-draw state (`COLOR_WRITEMASK`,
  polygon mode, rasterizer discard and framebuffer sRGB) so the guest draw
  cannot accidentally alter how the enhancement prepass is rendered;
- Vulkan consumes material-light revisions without waiting for the 4 ms
  animation/file-poll interval. Its normal pre-draw bind happens after the
  geometry observer, so the freshly published direction is recorded before the
  geometry draw that needs it;
- programmable VSH geometry no longer reconstructs the material TBN from
  perspective-divided NDC `(clip.xyz / w)`. NDC is nonlinear and changes the
  inferred basis with perspective. V6 instead uses `(clip.x, clip.y, clip.w)`
  as a game-agnostic pseudo-view coordinate: for conventional perspective
  transforms this is a fixed linear transform of view space and is much more
  stable under camera motion;
- V6 rejected nearly edge-on triangles before its camera-facing basis
  correction; V7 supersedes that correction entirely with a camera-independent
  tangent frame, described below;
- direct per-draw directions are not temporally blended with the previous draw,
  because the previous draw may intentionally be a different surface that uses
  the same replacement hash.

The remaining shared-prepass ceiling is narrower than V5: if one single NV2A
batch/multi-draw contains several incompatible surface orientations using the
same material stage, that one batch still receives one area-averaged material
direction. Removing even that limitation requires applying the normal map in
the actual geometry fragment draw rather than preprocessing a shared texture.

### Stable tangent basis and displacement/parallax (V7)

V7 fixes the remaining normal-map reversal visible when a surface is tilted far
enough to cross the camera-facing boundary and makes `_d` sidecars perform real
view-dependent relief mapping instead of the earlier single light-vector UV
offset.

- TBN orientation is now defined only by geometry winding plus UV handedness.
  The camera never negates the bitangent or normal. This removes the discrete
  tangent-space sign change that made a normal map appear to turn inside-out at
  a grazing angle.
- Two-sided guest geometry stays continuous by using the magnitude of the
  camera-axis normal component while preserving tangent/bitangent orientation.
  At edge-on incidence the Z component smoothly approaches zero rather than
  triggering a basis flip.
- The per-draw tangent-space camera direction is now a first-class material
  input. It is tracked when headlight, parallax, or specular behavior needs it,
  including when the user selects Directional lighting.
- `_d.png` is interpreted as a grayscale height map: white is raised/high and
  black is recessed/deep. The built-in pass uses adaptive 8-16 layer parallax
  occlusion/relief sampling plus intersection interpolation.
- Parallax uses the camera/view direction, never the material light direction.
  Normal, albedo, AO, and specular maps all sample the same displaced UV so the
  material layers remain registered.
- Grazing-angle parallax is faded and its denominator is bounded to prevent the
  extreme UV explosion normally produced as tangent-space view Z approaches
  zero. The existing **Parallax scale** slider controls relief depth; `0` fully
  disables the displacement pass.

This remains texture-coordinate displacement rather than actual vertex
tessellation: silhouettes and polygon geometry are unchanged. True geometric
displacement would require subdividing/moving the game's draw geometry or
integrating displacement into the real geometry pipeline.

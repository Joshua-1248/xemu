# Xemu Custom Fork — Free Camera

Renderer-level, game-agnostic free-camera work for the custom fork. The feature
implementation stays under `xemu-features/freecam/` and consumes the existing
feature-owned PGRAPH renderer wrapper rather than modifying native Xemu/QEMU
NV2A source.


## Milestone 8 — depth-aware fixed-function screen protection

Runtime evidence from the Max Payne regression scene showed that Milestone 7 was
installed and executing, but the screen-space safety gate was excluding almost
the entire fixed-function workload. A representative capture reported roughly
350k fixed-function draws while about 343k were counted as
`2D/non-perspective draws left unchanged`.

The old fixed-function guard treated **every** CMAT without perspective-W as a
screen/HUD pass. That is too broad: a title may submit real 3D/depth-buffered
geometry through a flat/affine fixed-function position path.

Milestone 8 fixes the precedence error and narrows the remaining fast-path rule:

- a draw that already passed a validated reconstructed 3D factorization can
  **never** be vetoed by the older flat/perspective-W screen heuristic;
- an exact PMAT path with a perspective-like guest projection is likewise
  treated as stronger 3D evidence;
- only unresolved no-perspective-W draws reach the fallback screen heuristic;
- among those unresolved draws, no depth testing/writing remains a
  high-confidence fixed screen/presentation pass and is left unchanged;
- unresolved flat draws with depth testing or depth writing are fail-open and
  continue into the compatibility transform;
- programmable fullscreen-triangle classification from Milestone 6 remains
  unchanged.

Advanced diagnostics now distinguish:

- **Flat no-depth screen draws left unchanged**
- **Flat depth-active draws allowed through** (plus the successfully transformed count)
- **Validated 3D draws bypassing flat-screen guard**

The second counter is the important regression signal. In the affected title it
should grow substantially, while the previously dominant broad 2D/non-perspective
counter should fall.

## Milestone 7 — General affine CMAT world-camera reconstruction

Milestone 7 addresses a runtime case where the Free Camera pose UI changed by
large amounts while the visible 3D world remained at the game's own camera.
Renderer diagnostics showed that fixed-function draws overwhelmingly fell from
Reconstructed View into the old post-projection compatibility path.

Reconstructed View now also tries a validated guest-PMAT split that previously
existed only in Projective compatibility mode. PMAT is accepted only when it is
perspective-like, produces an affine/nondegenerate pre-projection transform, and
recomposes CMAT within tolerance.

The earlier CMAT-only reconstruction required the pre-projection basis to be
close to orthonormal. That assumption is too strict when a title folds arbitrary
object/model scaling or shear into CMAT. Milestone 7 adds an exact
**general affine-perspective factorization** that uses the projective depth
relationship between CMAT columns 2 and 3 to construct a canonical perspective
matrix. The remaining matrix is then required to be affine and the product must
reconstruct the original CMAT before the free-camera delta is allowed.

For an accepted draw the transform is applied as:

```text
CMAT = A * P
CMAT' = A * FreecamDelta * P
```

This keeps the freecam before the perspective divide without requiring the
object/model part of `A` to be rigid or orthonormal. The path is attempted only
after the stricter camera-like CMAT split and MMAT-assisted recovery fail, and
before the old projective fallback. Invalid, oblique, non-perspective, or
numerically degenerate candidates fail closed.

Advanced renderer diagnostics expose **Validated PMAT view** and **General
affine CMAT view**, with attempt/rejected counts for both. In the regression
title, a successful M7 result should move a substantial portion of the former
`Reconstructed -> projective fallback` count into one of these new counters.

## Milestone 6 — fullscreen/screen-space pass protection

Some Xbox games render a final image or post-process with one oversized
right-angle triangle instead of a fullscreen quad. The primary regression case
is approximately `(0,0)`, `(2W,0)`, `(0,2H)`: the top and left edges are
axis-aligned, the ordinary `W x H` game image occupies the top-left portion at
its normal size, and the hypotenuse runs from bottom-left to top-right — the
user-observed `/` orientation. The triangle extends beyond the visible viewport
so that diagonal normally does not appear as part of the presented image.
Transforming that carrier triangle as if it were world geometry can pull the
`/` edge into view and make the image/post-process/video/composite move or warp
with the free camera.

Milestone 6 adds **Protect fullscreen / screen-space passes**, enabled by
default. Programmable draws with an unknown render signature are classified
before rasterization. A plausible three-vertex triangle is evaluated through
the original guest vertex shader for exactly those three vertices, then tested
against strict screen-space geometry criteria:

- all four viewport corners must be covered;
- the triangle must be substantially larger than the viewport;
- one vertex must form a near-right angle close to a viewport corner;
- the two legs must be near-horizontal/vertical and at least about 1.5x the
  viewport dimensions;
- output Z and W must be essentially flat.

A detected fullscreen triangle is left completely unchanged. Ordinary 3D draws
continue through the existing programmable post-VSH camera transform. The
classifier uses a bounded feature-owned signature cache for render signatures
that are proven not to be three-vertex candidates, so normal batched world draws
stay on the immediate hot path. Exact three-vertex candidates are deliberately
revalidated on every occurrence. A game can reuse the same VSH/render-state
signature with different vertex data, so this avoids a stale screen/world cache
decision pinning real geometry to the screen or transforming a later fullscreen
carrier.

Classification happens through the existing feature-owned geometry renderer
wrapper. Unknown programmable draws are deferred only until their complete draw
data is available. Vulkan already selects its pipeline in the later pre-draw
path; OpenGL explicitly refreshes the bound shader only when an unknown draw is
classified as 3D and receives the freecam transform late. No native/upstream
NV2A source change is required.

The same protection toggle leaves only high-confidence flat/no-depth
fixed-function screen/HUD passes unchanged in **both** camera modes. Flat
fixed-function draws that participate in depth testing or depth writing are
allowed through to the camera transform even when CMAT has no perspective-W.
Disabling the option restores the older Projective compatibility behavior for
troubleshooting.

### Milestone 6 diagnostics

Advanced renderer info now reports:

- programmable draws whose classification was deferred;
- fullscreen triangles detected;
- screen-space draws left unchanged.

## Milestone 5 — CMAT-inferred true-view reconstruction

Milestone 5 keeps both existing camera modes. Projective compatibility remains
unchanged. Reconstructed View is strengthened for titles such as Max Payne that
submit perspective fixed-function geometry through a usable `CMAT` while
keeping `PMAT` and `MMAT0` stale, singular, or otherwise unsuitable for camera
reconstruction.

### Camera modes

**Projective compatibility**

- Preserves the Milestone 3.1 transform for ordinary/world geometry.
- With screen-space protection enabled, recognized fullscreen/2D passes bypass
  that transform; disabling the protection restores the older behavior.
- Fixed-function draws use exact PMAT factorization when available and the
  projective CMAT-output fallback otherwise.
- Programmable VSH draws use the validated post-VSH synthetic tail.
- Best broad compatibility and remains the default mode.

**Reconstructed View (CMAT inferred)**

For an unskinned fixed-function perspective draw the mode now tries, in order:

1. **CMAT-only camera factorization.** The perspective structure of `CMAT` is
   decomposed into a camera-like affine pre-projection transform plus a
   perspective projection. The freecam delta is inserted between those two
   matrices, so translation/rotation occur in reconstructed 3D view space.
2. **MMAT0 + CMAT reconstruction.** The Milestone 4 path remains available as
   a secondary route when `MMAT0` genuinely describes the draw's model-view.
3. **Projective compatibility fallback.** Perspective-looking 3D draws that
   cannot be safely reconstructed retain the Milestone 3.1 fallback rather than
   disappearing.

The CMAT-only factorization supports off-center X/Y projection terms and one
XY-skew term, validates an orthonormal camera basis, checks that depth and clip-W
share the expected perspective axis, and recomposes the original `CMAT` before
accepting the split. The original draw is therefore unchanged when the freecam
pose is identity.

Obvious non-perspective fixed-function draws are left in guest screen space in
Reconstructed View instead of receiving the projective fallback. This keeps
HUD/overlay/2D work from being dragged, smeared, or repeated with the 3D world.

Skinned fixed-function draws continue to use MMAT0-3 as before. Ordinary 3D
programmable VSH draws continue to use the validated Milestone 3.1 post-VSH
compatibility tail; recognized fullscreen triangles bypass it. There is not yet
a generic safe pre-projection split for every arbitrary Xbox vertex program.

### Diagnostics

The Free Camera window reports:

- Reconstructed-view transforms
- CMAT-inferred true-view transforms
- CMAT perspective attempts / rejected candidates
- MMAT-assisted true-view transforms
- Reconstructed -> projective fallbacks
- 2D/non-perspective draws left unchanged
- Existing programmable post-VSH counters
- Deferred programmable screen-space classifications
- Fullscreen triangles detected / screen-space draws left unchanged

These counters distinguish an actual pre-projection camera transform from the
compatibility homography used by earlier builds.


### Cleaner window layout

The Free Camera window now separates `Controls` and `Info`. Renderer counters
from `NV2A renderer hook` through `All draws transformed` are hidden by default
and can be shown with **Show advanced renderer info**. **Capture mouse while
enabled** defaults to off so opening/enabling the tool does not immediately
claim relative mouse input. The window participates in the shared detachable
window system.

### Controls

- F10 enable/disable hotkey.
- Free Camera menu item lives under the custom-fork `Misc` menu.
- WASD movement; Q/E vertical movement.
- Relative mouse look with optional Y inversion.
- Z/C roll and arrow-key look fallback.
- Shift boost / Ctrl precision movement.
- Mouse-wheel speed adjustment.
- Home pose reset.
- Optional vertical-FOV override.
- Fullscreen/screen-space pass protection, enabled by default.

### Milestone 3.1 crash-safety retained

The post-VSH programmable path initializes all synthetic A/B/C muxes to valid
harmless operands and validates every generated tail instruction before guest
VSH state is changed. Invalid synthetic code is rejected as a transform failure
rather than reaching Xemu's GLSL translator.

## Projective compatibility model

The existing compatibility path acts after the game's camera has already
projected vertices. It is intentionally retained because it works with arbitrary
programmable camera math, but translation at that stage is a projective
reprojection rather than a true camera-space translation. That is why Max Payne
could look like an inner camera/portal stayed anchored while an outer camera
moved around it.

Reconstructed View avoids that behavior for accepted fixed-function draws by
inserting the camera delta before the recovered projection. Screen-space pass
protection is orthogonal to the camera mode: it prevents recognized presentation
geometry from receiving either camera transform in the first place.

## Remaining semantic ceiling

The pose is still an offset relative to the game's current view. CPU-side
portal/frustum/sector/LOD culling remains controlled by the Xbox game. A correct
renderer-side pre-projection transform can move the apparent camera naturally,
but it cannot create geometry the game never submitted for its original camera.
If a hard visibility boundary remains after CMAT-inferred transforms are active,
that boundary is game-side culling rather than a renderer projection artifact.

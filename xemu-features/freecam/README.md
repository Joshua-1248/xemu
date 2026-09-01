# Xemu Custom Fork — Free Camera

Renderer-level, game-agnostic free-camera work for the custom fork. The feature
implementation stays under `xemu-features/freecam/` and consumes the existing
feature-owned PGRAPH renderer wrapper rather than modifying native Xemu/QEMU
NV2A source.

## Milestone 5 — CMAT-inferred true-view reconstruction

Milestone 5 keeps both existing camera modes. Projective compatibility remains
unchanged. Reconstructed View is strengthened for titles such as Max Payne that
submit perspective fixed-function geometry through a usable `CMAT` while
keeping `PMAT` and `MMAT0` stale, singular, or otherwise unsuitable for camera
reconstruction.

### Camera modes

**Projective compatibility**

- Preserves the Milestone 3.1 behavior.
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

Skinned fixed-function draws continue to use MMAT0-3 as before. Programmable VSH
draws continue to use the validated Milestone 3.1 post-VSH compatibility tail;
there is not yet a generic safe pre-projection split for every arbitrary Xbox
vertex program.

### Diagnostics

The Free Camera window reports:

- Reconstructed-view transforms
- CMAT-inferred true-view transforms
- CMAT perspective attempts / rejected candidates
- MMAT-assisted true-view transforms
- Reconstructed -> projective fallbacks
- 2D/non-perspective draws left unchanged
- Existing programmable post-VSH counters

These counters distinguish an actual pre-projection camera transform from the
compatibility homography used by earlier builds.

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
inserting the camera delta before the recovered projection.

## Remaining semantic ceiling

The pose is still an offset relative to the game's current view. CPU-side
portal/frustum/sector/LOD culling remains controlled by the Xbox game. A correct
renderer-side pre-projection transform can move the apparent camera naturally,
but it cannot create geometry the game never submitted for its original camera.
If a hard visibility boundary remains after CMAT-inferred transforms are active,
that boundary is game-side culling rather than a renderer projection artifact.

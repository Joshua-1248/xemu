// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * xemu custom fork - renderer-level free camera public boundary
 *
 * The implementation is feature-owned and is installed through the existing
 * xemu-features PGRAPH renderer wrapper.  No upstream NV2A renderer source is
 * modified for the feature.
 */
#ifndef XEMU_FEATURES_FREECAM_H
#define XEMU_FEATURES_FREECAM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NV2AState;

typedef enum XemuFreecamRenderMode {
    /* Milestone 3 compatibility path: transform already-projected output. */
    XEMU_FREECAM_RENDER_PROJECTIVE = 0,
    /* Prefer a recovered fixed-function view/projection split. */
    XEMU_FREECAM_RENDER_RECONSTRUCTED_VIEW = 1,
} XemuFreecamRenderMode;

typedef struct XemuFreecamSettings {
    bool enabled;
    bool capture_mouse;
    bool invert_mouse_y;
    bool fov_override;
    bool protect_screen_space;
    uint32_t render_mode;
    float move_speed;
    float boost_multiplier;
    float precision_multiplier;
    float mouse_sensitivity_deg;
    float fov_degrees;
} XemuFreecamSettings;

typedef struct XemuFreecamPose {
    float position[3];
    float yaw_degrees;
    float pitch_degrees;
    float roll_degrees;
} XemuFreecamPose;

typedef struct XemuFreecamStatus {
    bool renderer_seen;
    bool enabled;
    bool draw_transform_active;
    uint64_t fixed_function_draws;
    uint64_t fixed_exact_transformed_draws;
    uint64_t fixed_projective_transformed_draws;
    uint64_t fixed_reconstructed_transformed_draws;
    uint64_t fixed_composite_inference_attempts;
    uint64_t fixed_composite_inference_rejected;
    uint64_t fixed_composite_inferred_transformed_draws;
    uint64_t fixed_mmat_reconstructed_transformed_draws;
    uint64_t fixed_pmat_inference_attempts;
    uint64_t fixed_pmat_inference_rejected;
    uint64_t fixed_pmat_reconstructed_transformed_draws;
    uint64_t fixed_affine_inference_attempts;
    uint64_t fixed_affine_inference_rejected;
    uint64_t fixed_affine_inferred_transformed_draws;
    uint64_t fixed_reconstructed_fallback_draws;
    uint64_t fixed_nonperspective_passthrough_draws;
    uint64_t fixed_nonperspective_depth_eligible_draws;
    uint64_t fixed_nonperspective_depth_transformed_draws;
    uint64_t fixed_validated_world_flat_guard_bypasses;
    uint64_t programmable_draws;
    uint64_t programmable_tail_eligible_draws;
    uint64_t programmable_transformed_draws;
    uint64_t programmable_no_room_draws;
    uint64_t programmable_no_constants_draws;
    uint64_t programmable_relative_constant_draws;
    uint64_t programmable_classification_deferred_draws;
    uint64_t programmable_screen_space_detected_draws;
    uint64_t programmable_screen_space_passthrough_draws;
    uint64_t transformed_draws;
    uint64_t transform_failures;
    XemuFreecamSettings settings;
    XemuFreecamPose pose;
} XemuFreecamStatus;

/* UI/control thread API. */
/* Lock-free hot-state query for frontend/renderer paths that only need to
 * know whether the feature is active. */
bool xemu_freecam_is_enabled(void);
void xemu_freecam_get_status(XemuFreecamStatus *status);
void xemu_freecam_get_settings(XemuFreecamSettings *settings);
void xemu_freecam_set_settings(const XemuFreecamSettings *settings);
void xemu_freecam_set_enabled(bool enabled);
void xemu_freecam_reset_pose(void);
void xemu_freecam_move_local(float right, float up, float forward);
void xemu_freecam_rotate(float yaw_degrees, float pitch_degrees,
                         float roll_degrees);

/* Renderer-thread hooks consumed by the feature-owned PGRAPH wrapper. */
void xemu_freecam_renderer_draw_begin(struct NV2AState *d);
/* Programmable draws whose screen-space role is not known yet are deferred
 * until the feature-owned geometry wrapper has the complete draw.  The
 * resolver returns true only when it applied the programmable transform late;
 * OpenGL callers then refresh the already-bound shader before drawing.
 *
 * `positions` is either NULL for a definitely non-triangle/world draw, or
 * exactly three original guest-VSH oPos vectors.  `classification_valid=false`
 * means observation failed; old freecam behavior is retained without caching
 * a guess. */
bool xemu_freecam_renderer_programmable_classification_pending(void);
bool xemu_freecam_renderer_resolve_programmable_draw(
    struct NV2AState *d, bool classification_valid,
    const float positions[3][4]);
void xemu_freecam_renderer_draw_end(struct NV2AState *d);
void xemu_freecam_renderer_abort_draw(struct NV2AState *d);

#ifdef __cplusplus
}
#endif

#endif

/*
 * xemu custom fork - renderer-level free camera, milestone 5
 *
 * Fixed-function NV2A draws support two user-selectable paths. The compatibility
 * path preserves Milestone 3 projective behavior. The reconstructed-view path
 * first factors a perspective camera directly from CMAT, so titles that keep
 * PMAT/MMAT stale or placeholder-like can still receive a true pre-projection
 * camera delta. MMAT0+CMAT recovery remains a secondary route, and the old
 * projective path remains the final 3D compatibility fallback.
 *
 * Programmable vertex shaders no longer depend on recognizing a game's camera
 * constants.  The feature temporarily appends a seven-slot post-VSH tail after
 * the guest's FINAL instruction.  That tail transforms the already-produced
 * screen-space oPos through a projective free-camera matrix, then restores the
 * exact guest program and constants after the draw.  The same injected guest
 * VSH runs through Xemu's normal GL/Vulkan translators, so the path is backend
 * independent and remains entirely feature-owned.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/thread.h"
#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/glsl/common.h"
#include "hw/xbox/nv2a/pgraph/vsh_regs.h"
#include "xemu-features/freecam/freecam.h"
#include "nv2a_vsh_disassembler.h"

#include <glib.h>
#include <math.h>
#include <string.h>

#define FREECAM_PI 3.14159265358979323846f
#define FREECAM_MATRIX_ROWS 4
#define FREECAM_SAVED_MATRICES 9 /* CMAT + MMAT0-3 + IMMAT0-3 */
#define FREECAM_POST_VSH_SLOTS 7
#define FREECAM_POST_VSH_SAVED_SLOTS (FREECAM_POST_VSH_SLOTS + 1)
#define FREECAM_REFERENCE_FOV_DEGREES 75.0f
#define FREECAM_PROGRAM_CACHE_SIZE 32

typedef struct FreecamVshFieldMapping {
    uint8_t subtoken;
    uint8_t start_bit;
    uint8_t bit_length;
} FreecamVshFieldMapping;

/* Mirrors the public NV2A token field layout used by vsh_get_field().  Keeping
 * the writer local to this feature avoids adding an upstream assembler API. */
static const FreecamVshFieldMapping freecam_vsh_field_mapping[] = {
    [FLD_ILU]          = { 1, 25, 3 },
    [FLD_MAC]          = { 1, 21, 4 },
    [FLD_CONST]        = { 1, 13, 8 },
    [FLD_V]            = { 1,  9, 4 },
    [FLD_A_NEG]        = { 1,  8, 1 },
    [FLD_A_SWZ_X]      = { 1,  6, 2 },
    [FLD_A_SWZ_Y]      = { 1,  4, 2 },
    [FLD_A_SWZ_Z]      = { 1,  2, 2 },
    [FLD_A_SWZ_W]      = { 1,  0, 2 },
    [FLD_A_R]          = { 2, 28, 4 },
    [FLD_A_MUX]        = { 2, 26, 2 },
    [FLD_B_NEG]        = { 2, 25, 1 },
    [FLD_B_SWZ_X]      = { 2, 23, 2 },
    [FLD_B_SWZ_Y]      = { 2, 21, 2 },
    [FLD_B_SWZ_Z]      = { 2, 19, 2 },
    [FLD_B_SWZ_W]      = { 2, 17, 2 },
    [FLD_B_R]          = { 2, 13, 4 },
    [FLD_B_MUX]        = { 2, 11, 2 },
    [FLD_C_NEG]        = { 2, 10, 1 },
    [FLD_C_SWZ_X]      = { 2,  8, 2 },
    [FLD_C_SWZ_Y]      = { 2,  6, 2 },
    [FLD_C_SWZ_Z]      = { 2,  4, 2 },
    [FLD_C_SWZ_W]      = { 2,  2, 2 },
    [FLD_C_R_HIGH]     = { 2,  0, 2 },
    [FLD_C_R_LOW]      = { 3, 30, 2 },
    [FLD_C_MUX]        = { 3, 28, 2 },
    [FLD_OUT_MAC_MASK] = { 3, 24, 4 },
    [FLD_OUT_R]        = { 3, 20, 4 },
    [FLD_OUT_ILU_MASK] = { 3, 16, 4 },
    [FLD_OUT_O_MASK]   = { 3, 12, 4 },
    [FLD_OUT_ORB]      = { 3, 11, 1 },
    [FLD_OUT_ADDRESS]  = { 3,  3, 8 },
    [FLD_OUT_MUX]      = { 3,  2, 1 },
    [FLD_A0X]          = { 3,  1, 1 },
    [FLD_FINAL]        = { 3,  0, 1 },
};

typedef struct FreecamState {
    QemuMutex lock;
    bool initialized;
    unsigned int revision;
    XemuFreecamSettings settings;
    XemuFreecamPose pose;

    /* Renderer-owned snapshot. The renderer only takes the mutex when the UI
     * revision changes, not once per draw. */
    unsigned int renderer_revision;
    XemuFreecamSettings renderer_settings;
    XemuFreecamPose renderer_pose;

    bool draw_active;
    bool fixed_saved_active;
    bool programmable_tail_saved_active;
    unsigned int saved_rows[FREECAM_SAVED_MATRICES][FREECAM_MATRIX_ROWS];
    uint32_t saved_constants[FREECAM_SAVED_MATRICES][FREECAM_MATRIX_ROWS][4];

    unsigned int programmable_tail_first_slot;
    uint32_t programmable_saved_program[FREECAM_POST_VSH_SAVED_SLOTS][VSH_TOKEN_SIZE];
    unsigned int programmable_saved_base;
    uint32_t programmable_saved_constants[FREECAM_MATRIX_ROWS][4];

    int renderer_seen;
    int draw_transform_active;
    int hot_enabled;
    uint64_t fixed_function_draws;
    uint64_t fixed_exact_transformed_draws;
    uint64_t fixed_projective_transformed_draws;
    uint64_t fixed_reconstructed_transformed_draws;
    uint64_t fixed_composite_inference_attempts;
    uint64_t fixed_composite_inference_rejected;
    uint64_t fixed_composite_inferred_transformed_draws;
    uint64_t fixed_mmat_reconstructed_transformed_draws;
    uint64_t fixed_reconstructed_fallback_draws;
    uint64_t fixed_nonperspective_passthrough_draws;
    uint64_t programmable_draws;
    uint64_t programmable_tail_eligible_draws;
    uint64_t programmable_transformed_draws;
    uint64_t programmable_no_room_draws;
    uint64_t programmable_no_constants_draws;
    uint64_t programmable_relative_constant_draws;
    uint64_t transformed_draws;
    uint64_t transform_failures;
} FreecamState;

static FreecamState g_freecam;
static gsize g_freecam_once;

static float clampf_fc(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void freecam_init_once(void)
{
    if (g_once_init_enter(&g_freecam_once)) {
        memset(&g_freecam, 0, sizeof(g_freecam));
        qemu_mutex_init(&g_freecam.lock);
        g_freecam.settings.enabled = false;
        g_freecam.settings.capture_mouse = true;
        g_freecam.settings.invert_mouse_y = false;
        g_freecam.settings.fov_override = false;
        g_freecam.settings.render_mode = XEMU_FREECAM_RENDER_PROJECTIVE;
        g_freecam.settings.move_speed = 5.0f;
        g_freecam.settings.boost_multiplier = 4.0f;
        g_freecam.settings.precision_multiplier = 0.25f;
        g_freecam.settings.mouse_sensitivity_deg = 0.10f;
        g_freecam.settings.fov_degrees = 75.0f;
        g_freecam.revision = 1;
        g_freecam.initialized = true;
        g_once_init_leave(&g_freecam_once, 1);
    }
}

static void freecam_bump_revision_locked(void)
{
    qatomic_inc(&g_freecam.revision);
}

static void freecam_basis(const XemuFreecamPose *pose,
                          float right[3], float up[3], float forward[3])
{
    const float yaw = pose->yaw_degrees * FREECAM_PI / 180.0f;
    const float pitch = pose->pitch_degrees * FREECAM_PI / 180.0f;
    const float roll = pose->roll_degrees * FREECAM_PI / 180.0f;
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cr = cosf(roll), sr = sinf(roll);

    float base_right[3] = { cy, 0.0f, -sy };
    float base_forward[3] = { sy * cp, sp, cy * cp };
    float base_up[3] = {
        -sy * sp,
        cp,
        -cy * sp,
    };

    /* Roll the camera's right/up plane around its forward axis. */
    for (int i = 0; i < 3; ++i) {
        right[i] = base_right[i] * cr + base_up[i] * sr;
        up[i] = base_up[i] * cr - base_right[i] * sr;
        forward[i] = base_forward[i];
    }
}

void xemu_freecam_get_settings(XemuFreecamSettings *settings)
{
    if (!settings) {
        return;
    }
    freecam_init_once();
    qemu_mutex_lock(&g_freecam.lock);
    *settings = g_freecam.settings;
    qemu_mutex_unlock(&g_freecam.lock);
}

void xemu_freecam_set_settings(const XemuFreecamSettings *settings)
{
    if (!settings) {
        return;
    }
    freecam_init_once();
    XemuFreecamSettings next = *settings;
    if (next.render_mode > XEMU_FREECAM_RENDER_RECONSTRUCTED_VIEW) {
        next.render_mode = XEMU_FREECAM_RENDER_PROJECTIVE;
    }
    next.move_speed = clampf_fc(next.move_speed, 0.0001f, 100000.0f);
    next.boost_multiplier = clampf_fc(next.boost_multiplier, 1.0f, 100.0f);
    next.precision_multiplier = clampf_fc(next.precision_multiplier, 0.001f, 1.0f);
    next.mouse_sensitivity_deg =
        clampf_fc(next.mouse_sensitivity_deg, 0.001f, 10.0f);
    next.fov_degrees = clampf_fc(next.fov_degrees, 10.0f, 170.0f);

    qemu_mutex_lock(&g_freecam.lock);
    g_freecam.settings = next;
    qatomic_set(&g_freecam.hot_enabled, next.enabled);
    freecam_bump_revision_locked();
    qemu_mutex_unlock(&g_freecam.lock);
}

void xemu_freecam_set_enabled(bool enabled)
{
    freecam_init_once();
    qemu_mutex_lock(&g_freecam.lock);
    if (g_freecam.settings.enabled != enabled) {
        g_freecam.settings.enabled = enabled;
        qatomic_set(&g_freecam.hot_enabled, enabled);
        freecam_bump_revision_locked();
    }
    qemu_mutex_unlock(&g_freecam.lock);
}

void xemu_freecam_reset_pose(void)
{
    freecam_init_once();
    qemu_mutex_lock(&g_freecam.lock);
    memset(&g_freecam.pose, 0, sizeof(g_freecam.pose));
    freecam_bump_revision_locked();
    qemu_mutex_unlock(&g_freecam.lock);
}

void xemu_freecam_move_local(float right_delta, float up_delta,
                             float forward_delta)
{
    freecam_init_once();
    if (!(isfinite(right_delta) && isfinite(up_delta) &&
          isfinite(forward_delta))) {
        return;
    }

    qemu_mutex_lock(&g_freecam.lock);
    float right[3], up[3], forward[3];
    freecam_basis(&g_freecam.pose, right, up, forward);
    for (int i = 0; i < 3; ++i) {
        g_freecam.pose.position[i] +=
            right[i] * right_delta + up[i] * up_delta +
            forward[i] * forward_delta;
    }
    freecam_bump_revision_locked();
    qemu_mutex_unlock(&g_freecam.lock);
}

void xemu_freecam_rotate(float yaw_degrees, float pitch_degrees,
                         float roll_degrees)
{
    freecam_init_once();
    if (!(isfinite(yaw_degrees) && isfinite(pitch_degrees) &&
          isfinite(roll_degrees))) {
        return;
    }

    qemu_mutex_lock(&g_freecam.lock);
    g_freecam.pose.yaw_degrees += yaw_degrees;
    g_freecam.pose.pitch_degrees = clampf_fc(
        g_freecam.pose.pitch_degrees + pitch_degrees, -89.9f, 89.9f);
    g_freecam.pose.roll_degrees += roll_degrees;

    /* Keep long sessions numerically tame without changing orientation. */
    if (fabsf(g_freecam.pose.yaw_degrees) > 3600.0f) {
        g_freecam.pose.yaw_degrees = fmodf(g_freecam.pose.yaw_degrees, 360.0f);
    }
    if (fabsf(g_freecam.pose.roll_degrees) > 3600.0f) {
        g_freecam.pose.roll_degrees = fmodf(g_freecam.pose.roll_degrees, 360.0f);
    }
    freecam_bump_revision_locked();
    qemu_mutex_unlock(&g_freecam.lock);
}

void xemu_freecam_get_status(XemuFreecamStatus *status)
{
    if (!status) {
        return;
    }
    freecam_init_once();
    memset(status, 0, sizeof(*status));
    status->renderer_seen = qatomic_read(&g_freecam.renderer_seen);
    status->draw_transform_active = qatomic_read(&g_freecam.draw_transform_active);
    status->fixed_function_draws = qatomic_read(&g_freecam.fixed_function_draws);
    status->fixed_exact_transformed_draws =
        qatomic_read(&g_freecam.fixed_exact_transformed_draws);
    status->fixed_projective_transformed_draws =
        qatomic_read(&g_freecam.fixed_projective_transformed_draws);
    status->fixed_reconstructed_transformed_draws =
        qatomic_read(&g_freecam.fixed_reconstructed_transformed_draws);
    status->fixed_composite_inference_attempts =
        qatomic_read(&g_freecam.fixed_composite_inference_attempts);
    status->fixed_composite_inference_rejected =
        qatomic_read(&g_freecam.fixed_composite_inference_rejected);
    status->fixed_composite_inferred_transformed_draws =
        qatomic_read(&g_freecam.fixed_composite_inferred_transformed_draws);
    status->fixed_mmat_reconstructed_transformed_draws =
        qatomic_read(&g_freecam.fixed_mmat_reconstructed_transformed_draws);
    status->fixed_reconstructed_fallback_draws =
        qatomic_read(&g_freecam.fixed_reconstructed_fallback_draws);
    status->fixed_nonperspective_passthrough_draws =
        qatomic_read(&g_freecam.fixed_nonperspective_passthrough_draws);
    status->programmable_draws = qatomic_read(&g_freecam.programmable_draws);
    status->programmable_tail_eligible_draws =
        qatomic_read(&g_freecam.programmable_tail_eligible_draws);
    status->programmable_transformed_draws =
        qatomic_read(&g_freecam.programmable_transformed_draws);
    status->programmable_no_room_draws =
        qatomic_read(&g_freecam.programmable_no_room_draws);
    status->programmable_no_constants_draws =
        qatomic_read(&g_freecam.programmable_no_constants_draws);
    status->programmable_relative_constant_draws =
        qatomic_read(&g_freecam.programmable_relative_constant_draws);
    status->transformed_draws = qatomic_read(&g_freecam.transformed_draws);
    status->transform_failures = qatomic_read(&g_freecam.transform_failures);
    qemu_mutex_lock(&g_freecam.lock);
    status->enabled = g_freecam.settings.enabled;
    status->settings = g_freecam.settings;
    status->pose = g_freecam.pose;
    qemu_mutex_unlock(&g_freecam.lock);
}

static void freecam_renderer_refresh_snapshot(void)
{
    freecam_init_once();
    unsigned int revision = qatomic_read(&g_freecam.revision);
    if (revision == g_freecam.renderer_revision) {
        return;
    }
    qemu_mutex_lock(&g_freecam.lock);
    g_freecam.renderer_settings = g_freecam.settings;
    g_freecam.renderer_pose = g_freecam.pose;
    g_freecam.renderer_revision = qatomic_read(&g_freecam.revision);
    qemu_mutex_unlock(&g_freecam.lock);
}

static float freecam_constant_float(PGRAPHState *pg, unsigned int row,
                                    unsigned int component)
{
    float value;
    uint32_t raw = pg->vsh_constants[row][component];
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static void freecam_matrix_read(PGRAPHState *pg, unsigned int base,
                                float out[4][4])
{
    /* NV2A c[] registers are GLSL matrix columns. Convert them into ordinary
     * row/column notation for the row-vector math used by the fixed-function
     * shader: p' = p * M. */
    for (unsigned int col = 0; col < 4; ++col) {
        for (unsigned int row = 0; row < 4; ++row) {
            out[row][col] = freecam_constant_float(pg, base + col, row);
        }
    }
}

static void freecam_matrix_write(PGRAPHState *pg, unsigned int base,
                                 const float in[4][4])
{
    for (unsigned int col = 0; col < 4; ++col) {
        for (unsigned int row = 0; row < 4; ++row) {
            uint32_t raw;
            float value = in[row][col];
            memcpy(&raw, &value, sizeof(raw));
            pg->vsh_constants[base + col][row] = raw;
        }
        pg->vsh_constants_dirty[base + col] = true;
    }
}

static void freecam_matrix_identity(float out[4][4])
{
    memset(out, 0, sizeof(float) * 16);
    for (int i = 0; i < 4; ++i) {
        out[i][i] = 1.0f;
    }
}

static void freecam_matrix_mul(const float a[4][4], const float b[4][4],
                               float out[4][4])
{
    float tmp[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[r][k] * b[k][c];
            }
            tmp[r][c] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static bool freecam_matrix_inverse(const float in[4][4], float out[4][4])
{
    float aug[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            aug[r][c] = in[r][c];
            aug[r][c + 4] = r == c ? 1.0f : 0.0f;
        }
    }

    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        float best = fabsf(aug[pivot][col]);
        for (int r = col + 1; r < 4; ++r) {
            float candidate = fabsf(aug[r][col]);
            if (candidate > best) {
                best = candidate;
                pivot = r;
            }
        }
        if (!(best > 1.0e-10f) || !isfinite(best)) {
            return false;
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                float t = aug[col][c];
                aug[col][c] = aug[pivot][c];
                aug[pivot][c] = t;
            }
        }
        float inv_pivot = 1.0f / aug[col][col];
        for (int c = 0; c < 8; ++c) {
            aug[col][c] *= inv_pivot;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            float factor = aug[r][col];
            for (int c = 0; c < 8; ++c) {
                aug[r][c] -= factor * aug[col][c];
            }
        }
    }

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r][c] = aug[r][c + 4];
            if (!isfinite(out[r][c])) {
                return false;
            }
        }
    }
    return true;
}

static bool freecam_matrix_all_finite(const float matrix[4][4])
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!isfinite(matrix[r][c])) {
                return false;
            }
        }
    }
    return true;
}

/* Recover the effective projection that follows MMAT0 for an unskinned
 * fixed-function draw.  Xemu's fixed VSH uses CMAT directly for position when
 * skinning is off, but MMAT0 still carries the model-view transform used by
 * lighting/fog/eye-space calculations.  If both describe the same draw then
 * CMAT = MMAT0 * P, so P = inverse(MMAT0) * CMAT.
 *
 * This is the key distinction from the Milestone 3 projective fallback: the
 * free-camera delta can be inserted before P, which keeps translation in view
 * space instead of warping an already-projected screen volume. */
static bool freecam_projection_looks_perspective(const float projection[4][4])
{
    float scale = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            scale = fmaxf(scale, fabsf(projection[r][c]));
        }
    }
    if (!(scale > 1.0e-8f) || !isfinite(scale)) {
        return false;
    }

    /* In row-vector form a perspective projection makes clip.w depend on the
     * incoming view-space Z (P[2][3]) and has little/no constant W term
     * (P[3][3]). Requiring usable X/Y scales also filters stale lighting
     * MMATs and most 2D/orthographic overlays before we treat MMAT0 as the
     * positional model-view transform. */
    const float min_axis = scale * 1.0e-5f;
    return fabsf(projection[0][0]) > min_axis &&
           fabsf(projection[1][1]) > min_axis &&
           fabsf(projection[2][3]) > min_axis &&
           fabsf(projection[3][3]) < scale * 0.10f;
}

static bool freecam_recover_fixed_projection(
    const float model_view[4][4], const float composite[4][4],
    float projection[4][4])
{
    float inverse_model_view[4][4];
    if (!freecam_matrix_inverse(model_view, inverse_model_view)) {
        return false;
    }
    freecam_matrix_mul(inverse_model_view, composite, projection);
    if (!freecam_matrix_all_finite(projection) ||
        !freecam_projection_looks_perspective(projection)) {
        return false;
    }

    /* Reject numerically unstable inversions. Recompose and compare against
     * the original CMAT using a scale-relative tolerance. */
    float recomposed[4][4];
    freecam_matrix_mul(model_view, projection, recomposed);
    float worst = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float scale = fmaxf(1.0f, fabsf(composite[r][c]));
            const float error = fabsf(recomposed[r][c] - composite[r][c]) / scale;
            worst = fmaxf(worst, error);
        }
    }
    return isfinite(worst) && worst <= 1.0e-3f;
}


/* CMAT-only perspective factorization.
 *
 * Xemu's fixed-function path emits position * CMAT, followed by perspective
 * divide and VPOFF.  Many Xbox titles (Max Payne included in the current test)
 * keep PMAT and MMAT0 unusable for positional reconstruction even though CMAT
 * itself still has the normal perspective-camera structure.
 *
 * For row-vector multiplication a conventional perspective projection has a
 * final column driven by view-space Z.  That lets us factor C = A * P directly:
 *   - C.col3 identifies the pre-projection Z axis and perspective-W scale;
 *   - C.col1, then C.col0, are Gram-Schmidt separated from Z to recover Y/X;
 *   - C.col2 must be parallel to Z apart from the homogeneous near/far term;
 *   - the translation row then determines A's camera-space translation and
 *     P's homogeneous depth offset.
 *
 * The factorization deliberately permits off-center X/Y projection terms and
 * one XY-skew term.  It then recomposes A*P and rejects the candidate unless it
 * reproduces CMAT closely.  This is a camera-like factorization, not a claim
 * that A is the game's original view matrix; critically, it gives us a stable
 * 3D space in which the freecam delta can be inserted before projection. */
static float freecam_vec3_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float freecam_vec3_length(const float v[3])
{
    return sqrtf(freecam_vec3_dot(v, v));
}

static bool freecam_vec3_normalize(const float in[3], float out[3],
                                   float *length_out)
{
    const float length = freecam_vec3_length(in);
    if (!(length > 1.0e-7f) || !isfinite(length)) {
        return false;
    }
    const float inv = 1.0f / length;
    for (int i = 0; i < 3; ++i) {
        out[i] = in[i] * inv;
    }
    if (length_out) {
        *length_out = length;
    }
    return true;
}

static float freecam_vec3_det_basis(const float x[3], const float y[3],
                                    const float z[3])
{
    const float cross[3] = {
        x[1] * y[2] - x[2] * y[1],
        x[2] * y[0] - x[0] * y[2],
        x[0] * y[1] - x[1] * y[0],
    };
    return freecam_vec3_dot(cross, z);
}

static bool freecam_composite_has_perspective_w(const float composite[4][4])
{
    const float w_axis[3] = {
        composite[0][3], composite[1][3], composite[2][3],
    };
    float matrix_scale = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            matrix_scale = fmaxf(matrix_scale, fabsf(composite[r][c]));
        }
    }
    const float w_length = freecam_vec3_length(w_axis);
    return isfinite(w_length) && isfinite(matrix_scale) &&
           matrix_scale > 1.0e-8f &&
           w_length > fmaxf(1.0e-6f, matrix_scale * 1.0e-7f);
}

static bool freecam_factor_composite_camera(
    const float composite[4][4], float pre_projection[4][4],
    float projection[4][4])
{
    if (!freecam_matrix_all_finite(composite) ||
        !freecam_composite_has_perspective_w(composite)) {
        return false;
    }

    const float c0[3] = {
        composite[0][0], composite[1][0], composite[2][0],
    };
    const float c1[3] = {
        composite[0][1], composite[1][1], composite[2][1],
    };
    const float c2[3] = {
        composite[0][2], composite[1][2], composite[2][2],
    };
    const float c3[3] = {
        composite[0][3], composite[1][3], composite[2][3],
    };

    float z_axis[3], perspective_w_scale;
    if (!freecam_vec3_normalize(c3, z_axis, &perspective_w_scale)) {
        return false;
    }

    /* Remove the off-center Z contribution from Y, then normalize Y. */
    const float p21 = freecam_vec3_dot(c1, z_axis);
    float y_raw[3];
    for (int i = 0; i < 3; ++i) {
        y_raw[i] = c1[i] - p21 * z_axis[i];
    }
    float y_axis[3], p11;
    if (!freecam_vec3_normalize(y_raw, y_axis, &p11)) {
        return false;
    }

    /* X may contain both off-center Z and projection skew along Y. */
    const float p20 = freecam_vec3_dot(c0, z_axis);
    float x_without_z[3];
    for (int i = 0; i < 3; ++i) {
        x_without_z[i] = c0[i] - p20 * z_axis[i];
    }
    const float p10 = freecam_vec3_dot(x_without_z, y_axis);
    float x_raw[3];
    for (int i = 0; i < 3; ++i) {
        x_raw[i] = x_without_z[i] - p10 * y_axis[i];
    }
    float x_axis[3], p00;
    if (!freecam_vec3_normalize(x_raw, x_axis, &p00)) {
        return false;
    }

    /* A usable camera basis must remain close to orthonormal after the
     * perspective terms above are removed. Reflections are valid, so accept
     * either handedness but reject a collapsed/ambiguous basis. */
    const float xy = fabsf(freecam_vec3_dot(x_axis, y_axis));
    const float xz = fabsf(freecam_vec3_dot(x_axis, z_axis));
    const float yz = fabsf(freecam_vec3_dot(y_axis, z_axis));
    const float handedness = fabsf(
        freecam_vec3_det_basis(x_axis, y_axis, z_axis));
    if (xy > 2.0e-3f || xz > 2.0e-3f || yz > 2.0e-3f ||
        handedness < 0.95f) {
        return false;
    }

    /* Perspective depth must use the same Z direction that drives clip W.
     * Oblique user clip planes can violate this; those draws stay on the old
     * compatibility path rather than receiving a bad camera split. */
    const float p22 = freecam_vec3_dot(c2, z_axis);
    float z_residual[3];
    for (int i = 0; i < 3; ++i) {
        z_residual[i] = c2[i] - p22 * z_axis[i];
    }
    const float residual = freecam_vec3_length(z_residual);
    const float depth_scale = fmaxf(1.0f, freecam_vec3_length(c2));
    if (!isfinite(residual) || residual > depth_scale * 2.0e-3f) {
        return false;
    }

    if (!(p00 > 1.0e-7f) || !(p11 > 1.0e-7f) ||
        !(perspective_w_scale > 1.0e-7f) ||
        !isfinite(p00) || !isfinite(p11) ||
        !isfinite(perspective_w_scale)) {
        return false;
    }

    /* Choose a rigid camera-like pre-projection basis and absorb any scalar
     * ambiguity into P. This keeps freecam translation in a consistent 3D
     * coordinate system instead of applying it to already-projected output. */
    freecam_matrix_identity(pre_projection);
    for (int r = 0; r < 3; ++r) {
        pre_projection[r][0] = x_axis[r];
        pre_projection[r][1] = y_axis[r];
        pre_projection[r][2] = z_axis[r];
    }

    const float tz = composite[3][3] / perspective_w_scale;
    const float ty = (composite[3][1] - tz * p21) / p11;
    const float tx = (composite[3][0] - ty * p10 - tz * p20) / p00;
    if (!(isfinite(tx) && isfinite(ty) && isfinite(tz))) {
        return false;
    }
    pre_projection[3][0] = tx;
    pre_projection[3][1] = ty;
    pre_projection[3][2] = tz;

    memset(projection, 0, sizeof(float) * 16);
    projection[0][0] = p00;
    projection[1][0] = p10;
    projection[2][0] = p20;
    projection[1][1] = p11;
    projection[2][1] = p21;
    projection[2][2] = p22;
    projection[2][3] = perspective_w_scale;
    projection[3][2] = composite[3][2] - tz * p22;

    if (!freecam_projection_looks_perspective(projection)) {
        return false;
    }

    /* The decomposition must be observationally equivalent to the original
     * draw before any freecam delta is inserted. */
    float recomposed[4][4];
    freecam_matrix_mul(pre_projection, projection, recomposed);
    float worst = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float scale = fmaxf(1.0f, fabsf(composite[r][c]));
            const float error = fabsf(recomposed[r][c] - composite[r][c]) /
                                scale;
            worst = fmaxf(worst, error);
        }
    }
    return isfinite(worst) && worst <= 2.0e-3f;
}

static void freecam_build_view_delta(const XemuFreecamPose *pose,
                                     float out[4][4])
{
    float right[3], up[3], forward[3];
    freecam_basis(pose, right, up, forward);
    freecam_matrix_identity(out);

    /* World/base-view -> freecam-view rotation: each output component is the
     * dot product with one camera basis vector, so the basis vectors are the
     * columns for row-vector multiplication. */
    for (int r = 0; r < 3; ++r) {
        out[r][0] = right[r];
        out[r][1] = up[r];
        out[r][2] = forward[r];
    }

    /* T(-camera_position) * R.  The translation row therefore contains the
     * camera position projected onto the rotated basis. */
    out[3][0] = -(pose->position[0] * right[0] +
                    pose->position[1] * right[1] +
                    pose->position[2] * right[2]);
    out[3][1] = -(pose->position[0] * up[0] +
                    pose->position[1] * up[1] +
                    pose->position[2] * up[2]);
    out[3][2] = -(pose->position[0] * forward[0] +
                    pose->position[1] * forward[1] +
                    pose->position[2] * forward[2]);
}

static bool freecam_pose_is_identity(const XemuFreecamPose *pose)
{
    const float eps = 1.0e-7f;
    return fabsf(pose->position[0]) < eps &&
           fabsf(pose->position[1]) < eps &&
           fabsf(pose->position[2]) < eps &&
           fabsf(pose->yaw_degrees) < eps &&
           fabsf(pose->pitch_degrees) < eps &&
           fabsf(pose->roll_degrees) < eps;
}

static float freecam_fov_scale(const XemuFreecamSettings *settings,
                               const float projection[4][4])
{
    if (!settings->fov_override) {
        return 1.0f;
    }
    float current_y = fabsf(projection[1][1]);
    float target_y = 1.0f / tanf(settings->fov_degrees * FREECAM_PI / 360.0f);
    if (!(current_y > 1.0e-5f) || !isfinite(current_y) ||
        !(target_y > 0.0f) || !isfinite(target_y)) {
        return 1.0f;
    }
    return clampf_fc(target_y / current_y, 0.05f, 20.0f);
}

static void freecam_scale_clip_xy(float matrix[4][4], float scale)
{
    if (fabsf(scale - 1.0f) < 1.0e-6f) {
        return;
    }
    for (int r = 0; r < 4; ++r) {
        matrix[r][0] *= scale;
        matrix[r][1] *= scale;
    }
}

static void freecam_save_matrix(PGRAPHState *pg, int index, unsigned int base)
{
    g_freecam.saved_rows[index][0] = base + 0;
    g_freecam.saved_rows[index][1] = base + 1;
    g_freecam.saved_rows[index][2] = base + 2;
    g_freecam.saved_rows[index][3] = base + 3;
    for (int c = 0; c < 4; ++c) {
        memcpy(g_freecam.saved_constants[index][c],
               pg->vsh_constants[base + c],
               sizeof(g_freecam.saved_constants[index][c]));
    }
}

static void freecam_restore_saved(PGRAPHState *pg)
{
    if (!g_freecam.draw_active) {
        return;
    }

    if (g_freecam.fixed_saved_active) {
        for (int i = 0; i < FREECAM_SAVED_MATRICES; ++i) {
            for (int c = 0; c < 4; ++c) {
                unsigned int row = g_freecam.saved_rows[i][c];
                memcpy(pg->vsh_constants[row], g_freecam.saved_constants[i][c],
                       sizeof(g_freecam.saved_constants[i][c]));
                pg->vsh_constants_dirty[row] = true;
            }
        }
        g_freecam.fixed_saved_active = false;
    }

    if (g_freecam.programmable_tail_saved_active) {
        const unsigned int first = g_freecam.programmable_tail_first_slot;
        for (unsigned int i = 0; i < FREECAM_POST_VSH_SAVED_SLOTS; ++i) {
            memcpy(pg->program_data[first + i],
                   g_freecam.programmable_saved_program[i],
                   sizeof(g_freecam.programmable_saved_program[i]));
        }
        pg->program_data_dirty = true;

        const unsigned int base = g_freecam.programmable_saved_base;
        for (int c = 0; c < FREECAM_MATRIX_ROWS; ++c) {
            memcpy(pg->vsh_constants[base + c],
                   g_freecam.programmable_saved_constants[c],
                   sizeof(g_freecam.programmable_saved_constants[c]));
            pg->vsh_constants_dirty[base + c] = true;
        }
        g_freecam.programmable_tail_saved_active = false;
    }

    g_freecam.draw_active = false;
    qatomic_set(&g_freecam.draw_transform_active, 0);
}

static bool freecam_surface_metrics(PGRAPHState *pg, float *width,
                                    float *height, float *zmax)
{
    unsigned int aa_width = 1, aa_height = 1;
    pgraph_apply_anti_aliasing_factor(pg, &aa_width, &aa_height);
    if (!aa_width || !aa_height || !pg->surface_binding_dim.width ||
        !pg->surface_binding_dim.height) {
        return false;
    }

    *width = (float)pg->surface_binding_dim.width / (float)aa_width;
    *height = (float)pg->surface_binding_dim.height / (float)aa_height;

    float clip_range[4];
    pgraph_glsl_set_clip_range_uniform_value(pg, clip_range);
    *zmax = clip_range[1];
    return isfinite(*width) && isfinite(*height) && isfinite(*zmax) &&
           *width > 0.0f && *height > 0.0f && *zmax > 0.0f;
}

static void freecam_build_reference_projection(float aspect, float fov_degrees,
                                               float out[4][4])
{
    aspect = clampf_fc(aspect, 0.25f, 8.0f);
    const float fov = clampf_fc(fov_degrees, 10.0f, 170.0f) *
                      FREECAM_PI / 180.0f;
    const float y_scale = 1.0f / tanf(fov * 0.5f);
    const float x_scale = y_scale / aspect;
    const float z_near = 0.1f;
    const float z_far = 10000.0f;
    const float q = z_far / (z_far - z_near);

    freecam_matrix_identity(out);
    out[0][0] = x_scale;
    out[1][1] = y_scale;
    out[2][2] = q;
    out[2][3] = 1.0f;
    out[3][2] = -z_near * q;
    out[3][3] = 0.0f;
}

static bool freecam_build_generic_clip_transform(
    PGRAPHState *pg, const XemuFreecamSettings *settings,
    const XemuFreecamPose *pose, float out[4][4])
{
    float width, height, zmax;
    if (!freecam_surface_metrics(pg, &width, &height, &zmax)) {
        return false;
    }
    (void)zmax;

    const float aspect = width / height;
    float projection[4][4], target_projection[4][4];
    float inverse_projection[4][4], view_delta[4][4], tmp[4][4];
    freecam_build_reference_projection(aspect, FREECAM_REFERENCE_FOV_DEGREES,
                                       projection);
    freecam_build_reference_projection(
        aspect, settings->fov_override ? settings->fov_degrees
                                       : FREECAM_REFERENCE_FOV_DEGREES,
        target_projection);
    if (!freecam_matrix_inverse(projection, inverse_projection)) {
        return false;
    }
    freecam_build_view_delta(pose, view_delta);
    freecam_matrix_mul(inverse_projection, view_delta, tmp);
    freecam_matrix_mul(tmp, target_projection, out);
    return true;
}

/* Build a matrix that acts directly on the fixed-function CMAT output.  Xemu's
 * fixed VSH converts raw = position*CMAT into host clip as:
 *   x = 2*raw.x/W + raw.w*(2*VPOFF.x/W - 1)
 *   y = 2*raw.y/H + raw.w*(2*VPOFF.y/H - 1)
 *   z = raw.z/zmax
 *   w = raw.w
 * Conjugating the generic clip transform through that exact linear mapping
 * avoids needing an invertible guest PMAT. */
static bool freecam_build_fixed_raw_post_transform(
    PGRAPHState *pg, const XemuFreecamSettings *settings,
    const XemuFreecamPose *pose, float out[4][4])
{
    float width, height, zmax;
    if (!freecam_surface_metrics(pg, &width, &height, &zmax)) {
        return false;
    }

    const float off_x = freecam_constant_float(
        pg, NV_IGRAPH_XF_XFCTX_VPOFF, 0);
    const float off_y = freecam_constant_float(
        pg, NV_IGRAPH_XF_XFCTX_VPOFF, 1);
    if (!(isfinite(off_x) && isfinite(off_y))) {
        return false;
    }

    float raw_to_clip[4][4], clip_to_raw[4][4];
    float clip_delta[4][4], tmp[4][4];
    freecam_matrix_identity(raw_to_clip);
    raw_to_clip[0][0] = 2.0f / width;
    raw_to_clip[1][1] = 2.0f / height;
    raw_to_clip[2][2] = 1.0f / zmax;
    raw_to_clip[3][0] = 2.0f * off_x / width - 1.0f;
    raw_to_clip[3][1] = 2.0f * off_y / height - 1.0f;

    if (!freecam_matrix_inverse(raw_to_clip, clip_to_raw) ||
        !freecam_build_generic_clip_transform(pg, settings, pose, clip_delta)) {
        return false;
    }
    freecam_matrix_mul(raw_to_clip, clip_delta, tmp);
    freecam_matrix_mul(tmp, clip_to_raw, out);
    return true;
}

/* Programmable shaders leave oPos in NV2A screen space.  The host translator
 * later normalizes it with surfaceSize/clipRange and multiplies xyz by w.
 * Convert screen coordinates to normalized clip, apply the generic camera
 * delta, then convert back to homogeneous screen coordinates.  Four DPHs in
 * the injected tail evaluate the columns of this matrix with implicit q.w=1. */
static bool freecam_build_programmable_screen_matrix(
    PGRAPHState *pg, const XemuFreecamSettings *settings,
    const XemuFreecamPose *pose, float out[4][4])
{
    float width, height, zmax;
    if (!freecam_surface_metrics(pg, &width, &height, &zmax)) {
        return false;
    }

    float screen_to_ndc[4][4], ndc_to_screen[4][4];
    float clip_delta[4][4], tmp[4][4];
    freecam_matrix_identity(screen_to_ndc);
    screen_to_ndc[0][0] = 2.0f / width;
    screen_to_ndc[1][1] = 2.0f / height;
    screen_to_ndc[2][2] = 1.0f / zmax;
    screen_to_ndc[3][0] = -1.0f;
    screen_to_ndc[3][1] = -1.0f;

    freecam_matrix_identity(ndc_to_screen);
    ndc_to_screen[0][0] = width * 0.5f;
    ndc_to_screen[1][1] = height * 0.5f;
    ndc_to_screen[2][2] = zmax;
    ndc_to_screen[3][0] = width * 0.5f;
    ndc_to_screen[3][1] = height * 0.5f;

    if (!freecam_build_generic_clip_transform(pg, settings, pose, clip_delta)) {
        return false;
    }
    freecam_matrix_mul(screen_to_ndc, clip_delta, tmp);
    freecam_matrix_mul(tmp, ndc_to_screen, out);
    return true;
}

static void freecam_vsh_set_field(uint32_t token[VSH_TOKEN_SIZE],
                                  VshFieldName field, uint32_t value)
{
    const FreecamVshFieldMapping *m = &freecam_vsh_field_mapping[field];
    const uint32_t mask = ((UINT32_C(1) << m->bit_length) - 1u) << m->start_bit;
    token[m->subtoken] = (token[m->subtoken] & ~mask) |
        ((value << m->start_bit) & mask);
}

static void freecam_vsh_set_swizzle(uint32_t token[VSH_TOKEN_SIZE],
                                    VshFieldName first, uint32_t x,
                                    uint32_t y, uint32_t z, uint32_t w)
{
    freecam_vsh_set_field(token, first + 0, x);
    freecam_vsh_set_field(token, first + 1, y);
    freecam_vsh_set_field(token, first + 2, z);
    freecam_vsh_set_field(token, first + 3, w);
}

static void freecam_vsh_token_init(uint32_t token[VSH_TOKEN_SIZE])
{
    memset(token, 0, VSH_TOKEN_SIZE * sizeof(*token));

    /* Xemu's GLSL translator decodes input C for every non-NOP token before
     * it knows whether the selected opcode actually consumes C.  A zero mux
     * therefore means PARAM_UNKNOWN and trips vsh-prog.c's safety assertion.
     * Keep all otherwise-unused inputs pointed at harmless R0 so every
     * feature-generated token is valid for both the CPU parser and the
     * GL/Vulkan GLSL translator.  Instruction builders override the inputs
     * they really consume below. */
    freecam_vsh_set_field(token, FLD_A_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_B_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_C_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_A_R, 0);
    freecam_vsh_set_field(token, FLD_B_R, 0);
    freecam_vsh_set_field(token, FLD_C_R_HIGH, 0);
    freecam_vsh_set_field(token, FLD_C_R_LOW, 0);

    freecam_vsh_set_swizzle(token, FLD_A_SWZ_X,
                            SWIZZLE_X, SWIZZLE_Y, SWIZZLE_Z, SWIZZLE_W);
    freecam_vsh_set_swizzle(token, FLD_B_SWZ_X,
                            SWIZZLE_X, SWIZZLE_Y, SWIZZLE_Z, SWIZZLE_W);
    freecam_vsh_set_swizzle(token, FLD_C_SWZ_X,
                            SWIZZLE_X, SWIZZLE_Y, SWIZZLE_Z, SWIZZLE_W);
}

static uint32_t freecam_vsh_get_field(const uint32_t token[VSH_TOKEN_SIZE],
                                      VshFieldName field)
{
    const FreecamVshFieldMapping *m = &freecam_vsh_field_mapping[field];
    return (token[m->subtoken] >> m->start_bit) &
        ((UINT32_C(1) << m->bit_length) - 1u);
}

static bool freecam_vsh_input_mux_valid(uint32_t mux)
{
    return mux >= PARAM_R && mux <= PARAM_C;
}

static bool freecam_vsh_generated_token_valid(
    const uint32_t token[VSH_TOKEN_SIZE])
{
    /* This deliberately mirrors the stricter requirement of Xemu's GLSL VSH
     * translator, not merely nv2a_vsh_cpu's parser.  If a future synthetic
     * instruction builder forgets to initialize an operand mux, reject the
     * programmable freecam draw rather than allowing the renderer to abort. */
    if (!freecam_vsh_input_mux_valid(
            freecam_vsh_get_field(token, FLD_A_MUX)) ||
        !freecam_vsh_input_mux_valid(
            freecam_vsh_get_field(token, FLD_B_MUX)) ||
        !freecam_vsh_input_mux_valid(
            freecam_vsh_get_field(token, FLD_C_MUX))) {
        return false;
    }

    Nv2aVshStep step;
    memset(&step, 0, sizeof(step));
    return nv2a_vsh_parse_step(&step, token) == NV2AVPR_SUCCESS;
}

static void freecam_vsh_make_dph(uint32_t token[VSH_TOKEN_SIZE],
                                 unsigned int context_index,
                                 unsigned int component)
{
    static const uint32_t masks[4] = { 8, 4, 2, 1 };
    freecam_vsh_token_init(token);
    freecam_vsh_set_field(token, FLD_MAC, MAC_DPH);
    freecam_vsh_set_field(token, FLD_A_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_A_R, 12); /* R12 mirrors oPos. */
    freecam_vsh_set_field(token, FLD_B_MUX, PARAM_C);
    freecam_vsh_set_field(token, FLD_CONST, context_index);
    freecam_vsh_set_field(token, FLD_OUT_R, 0);
    freecam_vsh_set_field(token, FLD_OUT_MAC_MASK, masks[component]);
}

static void freecam_vsh_make_rcp_denominator(uint32_t token[VSH_TOKEN_SIZE])
{
    freecam_vsh_token_init(token);
    freecam_vsh_set_field(token, FLD_ILU, ILU_RCP);
    freecam_vsh_set_field(token, FLD_C_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_C_R_HIGH, 0);
    freecam_vsh_set_field(token, FLD_C_R_LOW, 0);
    freecam_vsh_set_swizzle(token, FLD_C_SWZ_X,
                            SWIZZLE_W, SWIZZLE_W, SWIZZLE_W, SWIZZLE_W);
    freecam_vsh_set_field(token, FLD_OUT_R, 1);
    freecam_vsh_set_field(token, FLD_OUT_ILU_MASK, 8); /* R1.x */
}

static void freecam_vsh_make_divide_xyz(uint32_t token[VSH_TOKEN_SIZE])
{
    freecam_vsh_token_init(token);
    freecam_vsh_set_field(token, FLD_MAC, MAC_MUL);
    freecam_vsh_set_field(token, FLD_A_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_A_R, 0);
    freecam_vsh_set_field(token, FLD_B_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_B_R, 1);
    freecam_vsh_set_swizzle(token, FLD_B_SWZ_X,
                            SWIZZLE_X, SWIZZLE_X, SWIZZLE_X, SWIZZLE_X);
    freecam_vsh_set_field(token, FLD_OUT_O_MASK, 14); /* xyz */
    freecam_vsh_set_field(token, FLD_OUT_ORB, OUTPUT_O);
    freecam_vsh_set_field(token, FLD_OUT_ADDRESS, 0); /* oPos */
    freecam_vsh_set_field(token, FLD_OUT_MUX, OMUX_MAC);
}

static void freecam_vsh_make_update_w(uint32_t token[VSH_TOKEN_SIZE])
{
    freecam_vsh_token_init(token);
    freecam_vsh_set_field(token, FLD_MAC, MAC_MUL);
    freecam_vsh_set_field(token, FLD_A_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_A_R, 12); /* old oPos.w survives xyz write */
    freecam_vsh_set_swizzle(token, FLD_A_SWZ_X,
                            SWIZZLE_W, SWIZZLE_W, SWIZZLE_W, SWIZZLE_W);
    freecam_vsh_set_field(token, FLD_B_MUX, PARAM_R);
    freecam_vsh_set_field(token, FLD_B_R, 0);
    freecam_vsh_set_swizzle(token, FLD_B_SWZ_X,
                            SWIZZLE_W, SWIZZLE_W, SWIZZLE_W, SWIZZLE_W);
    freecam_vsh_set_field(token, FLD_OUT_O_MASK, 1); /* w */
    freecam_vsh_set_field(token, FLD_OUT_ORB, OUTPUT_O);
    freecam_vsh_set_field(token, FLD_OUT_ADDRESS, 0); /* oPos */
    freecam_vsh_set_field(token, FLD_OUT_MUX, OMUX_MAC);
    freecam_vsh_set_field(token, FLD_FINAL, 1);
}

static bool freecam_program_layout(PGRAPHState *pg, unsigned int *program_start,
                                   unsigned int *final_slot,
                                   bool used_constants[NV2A_VERTEXSHADER_CONSTANTS],
                                   bool *has_relative_context)
{
    *program_start = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
        NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
    if (*program_start >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        return false;
    }

    memset(used_constants, 0,
           NV2A_VERTEXSHADER_CONSTANTS * sizeof(*used_constants));
    *has_relative_context = false;

    for (unsigned int slot = *program_start;
         slot < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; ++slot) {
        Nv2aVshStep step;
        memset(&step, 0, sizeof(step));
        if (nv2a_vsh_parse_step(&step, pg->program_data[slot]) !=
            NV2AVPR_SUCCESS) {
            return false;
        }
        const Nv2aVshOperation *ops[2] = { &step.mac, &step.ilu };
        for (int which = 0; which < 2; ++which) {
            const Nv2aVshOperation *op = ops[which];
            for (int i = 0; i < 3; ++i) {
                const Nv2aVshInput *in = &op->inputs[i];
                if (in->type != NV2ART_CONTEXT) {
                    continue;
                }
                if (in->is_relative) {
                    *has_relative_context = true;
                } else if (in->index < NV2A_VERTEXSHADER_CONSTANTS) {
                    used_constants[in->index] = true;
                }
            }
            for (int i = 0; i < 2; ++i) {
                const Nv2aVshOutput *out = &op->outputs[i];
                if (out->type == NV2ART_CONTEXT &&
                    out->index < NV2A_VERTEXSHADER_CONSTANTS) {
                    used_constants[out->index] = true;
                }
            }
        }
        if (step.is_final) {
            *final_slot = slot;
            return true;
        }
    }
    return false;
}

static bool freecam_find_constant_scratch(
    const bool used_constants[NV2A_VERTEXSHADER_CONSTANTS], unsigned int *base)
{
    for (int candidate = NV2A_VERTEXSHADER_CONSTANTS - FREECAM_MATRIX_ROWS;
         candidate >= 0; --candidate) {
        bool free = true;
        for (int i = 0; i < FREECAM_MATRIX_ROWS; ++i) {
            if (used_constants[candidate + i]) {
                free = false;
                break;
            }
        }
        if (free) {
            *base = (unsigned int)candidate;
            return true;
        }
    }
    return false;
}

typedef enum FreecamProgramCacheResult {
    FREECAM_PROGRAM_CACHE_OK = 0,
    FREECAM_PROGRAM_CACHE_BAD_PROGRAM,
    FREECAM_PROGRAM_CACHE_NO_ROOM,
    FREECAM_PROGRAM_CACHE_RELATIVE_CONTEXT,
    FREECAM_PROGRAM_CACHE_NO_CONSTANTS,
    FREECAM_PROGRAM_CACHE_BAD_TAIL,
} FreecamProgramCacheResult;

typedef struct FreecamProgramCacheEntry {
    bool valid;
    unsigned int program_start;
    unsigned int final_slot;
    unsigned int constant_base;
    uint64_t source_hash;
    uint64_t last_use;
    FreecamProgramCacheResult result;
    uint32_t source[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][4];
    uint32_t generated_tail[FREECAM_POST_VSH_SLOTS][VSH_TOKEN_SIZE];
} FreecamProgramCacheEntry;

static FreecamProgramCacheEntry
    g_freecam_program_cache[FREECAM_PROGRAM_CACHE_SIZE];
static uint64_t g_freecam_program_cache_clock;

static FreecamProgramCacheEntry *freecam_program_cache_get(PGRAPHState *pg)
{
    unsigned int program_start = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
        NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
    if (program_start >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        return NULL;
    }

    const size_t source_slots = NV2A_MAX_TRANSFORM_PROGRAM_LENGTH - program_start;
    const size_t source_bytes = source_slots * sizeof(pg->program_data[0]);
    const uint8_t *source = (const uint8_t *)&pg->program_data[program_start][0];
    const uint64_t hash = fast_hash(source, source_bytes);
    const uint64_t stamp = ++g_freecam_program_cache_clock;

    for (int i = 0; i < FREECAM_PROGRAM_CACHE_SIZE; ++i) {
        FreecamProgramCacheEntry *entry = &g_freecam_program_cache[i];
        if (!entry->valid || entry->program_start != program_start ||
            entry->source_hash != hash) {
            continue;
        }
        if (memcmp(entry->source, source, source_bytes) == 0) {
            entry->last_use = stamp;
            return entry;
        }
    }

    FreecamProgramCacheEntry *entry = NULL;
    for (int i = 0; i < FREECAM_PROGRAM_CACHE_SIZE; ++i) {
        FreecamProgramCacheEntry *candidate = &g_freecam_program_cache[i];
        if (!candidate->valid) {
            entry = candidate;
            break;
        }
        if (!entry || candidate->last_use < entry->last_use) {
            entry = candidate;
        }
    }
    assert(entry != NULL);
    memset(entry, 0, sizeof(*entry));
    entry->valid = true;
    entry->program_start = program_start;
    entry->source_hash = hash;
    entry->last_use = stamp;
    entry->result = FREECAM_PROGRAM_CACHE_BAD_PROGRAM;
    memcpy(entry->source, source, source_bytes);

    unsigned int parsed_start = 0, final_slot = 0;
    bool used_constants[NV2A_VERTEXSHADER_CONSTANTS];
    bool has_relative_context = false;
    if (!freecam_program_layout(pg, &parsed_start, &final_slot,
                                used_constants, &has_relative_context) ||
        parsed_start != program_start) {
        return entry;
    }
    entry->final_slot = final_slot;

    if (final_slot + FREECAM_POST_VSH_SLOTS >=
        NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        entry->result = FREECAM_PROGRAM_CACHE_NO_ROOM;
        return entry;
    }
    if (has_relative_context) {
        entry->result = FREECAM_PROGRAM_CACHE_RELATIVE_CONTEXT;
        return entry;
    }
    if (!freecam_find_constant_scratch(used_constants, &entry->constant_base)) {
        entry->result = FREECAM_PROGRAM_CACHE_NO_CONSTANTS;
        return entry;
    }

    for (unsigned int component = 0; component < 4; ++component) {
        freecam_vsh_make_dph(entry->generated_tail[component],
                             entry->constant_base + component, component);
    }
    freecam_vsh_make_rcp_denominator(entry->generated_tail[4]);
    freecam_vsh_make_divide_xyz(entry->generated_tail[5]);
    freecam_vsh_make_update_w(entry->generated_tail[6]);
    for (unsigned int i = 0; i < FREECAM_POST_VSH_SLOTS; ++i) {
        if (!freecam_vsh_generated_token_valid(entry->generated_tail[i])) {
            entry->result = FREECAM_PROGRAM_CACHE_BAD_TAIL;
            return entry;
        }
    }
    entry->result = FREECAM_PROGRAM_CACHE_OK;
    return entry;
}

static bool freecam_transform_programmable_draw(PGRAPHState *pg)
{
    FreecamProgramCacheEntry *cached = freecam_program_cache_get(pg);
    if (!cached) {
        qatomic_inc(&g_freecam.transform_failures);
        return false;
    }
    switch (cached->result) {
    case FREECAM_PROGRAM_CACHE_OK:
        break;
    case FREECAM_PROGRAM_CACHE_NO_ROOM:
        qatomic_inc(&g_freecam.programmable_no_room_draws);
        return false;
    case FREECAM_PROGRAM_CACHE_RELATIVE_CONTEXT:
        qatomic_inc(&g_freecam.programmable_relative_constant_draws);
        return false;
    case FREECAM_PROGRAM_CACHE_NO_CONSTANTS:
        qatomic_inc(&g_freecam.programmable_no_constants_draws);
        return false;
    case FREECAM_PROGRAM_CACHE_BAD_PROGRAM:
    case FREECAM_PROGRAM_CACHE_BAD_TAIL:
    default:
        qatomic_inc(&g_freecam.transform_failures);
        return false;
    }

    const unsigned int final_slot = cached->final_slot;
    const unsigned int constant_base = cached->constant_base;

    float screen_matrix[4][4];
    if (!freecam_build_programmable_screen_matrix(
            pg, &g_freecam.renderer_settings, &g_freecam.renderer_pose,
            screen_matrix)) {
        qatomic_inc(&g_freecam.transform_failures);
        return false;
    }

    qatomic_inc(&g_freecam.programmable_tail_eligible_draws);
    g_freecam.programmable_tail_first_slot = final_slot;
    g_freecam.programmable_saved_base = constant_base;
    for (unsigned int i = 0; i < FREECAM_POST_VSH_SAVED_SLOTS; ++i) {
        memcpy(g_freecam.programmable_saved_program[i],
               pg->program_data[final_slot + i],
               sizeof(g_freecam.programmable_saved_program[i]));
    }
    for (int c = 0; c < FREECAM_MATRIX_ROWS; ++c) {
        memcpy(g_freecam.programmable_saved_constants[c],
               pg->vsh_constants[constant_base + c],
               sizeof(g_freecam.programmable_saved_constants[c]));
    }

    /* Continue past the guest's original FINAL and append the validated
     * post-VSH tail.  No guest state is touched until the entire synthetic
     * program fragment has passed the feature-side safety gate above. */
    freecam_vsh_set_field(pg->program_data[final_slot], FLD_FINAL, 0);
    for (unsigned int i = 0; i < FREECAM_POST_VSH_SLOTS; ++i) {
        memcpy(pg->program_data[final_slot + 1 + i], cached->generated_tail[i],
               sizeof(cached->generated_tail[i]));
    }
    pg->program_data_dirty = true;

    freecam_matrix_write(pg, constant_base, screen_matrix);

    g_freecam.programmable_tail_saved_active = true;
    g_freecam.draw_active = true;
    qatomic_inc(&g_freecam.programmable_transformed_draws);
    qatomic_inc(&g_freecam.transformed_draws);
    qatomic_set(&g_freecam.draw_transform_active, 1);
    return true;
}

void xemu_freecam_renderer_draw_begin(NV2AState *d)
{
    freecam_init_once();
    if (!d || !qatomic_read(&g_freecam.hot_enabled)) {
        /* Disabled freecam is deliberately a single hot flag test: no PGRAPH
         * register reads, snapshot mutex checks, shader parsing, or diagnostic
         * counter traffic on normal renderer draws. */
        return;
    }
    PGRAPHState *pg = &d->pgraph;
    freecam_renderer_refresh_snapshot();

    qatomic_set(&g_freecam.renderer_seen, 1);

    const unsigned int transform_mode = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_D), NV_PGRAPH_CSV0_D_MODE);
    if (transform_mode == 0) {
        qatomic_inc(&g_freecam.fixed_function_draws);
    } else if (transform_mode == 2) {
        qatomic_inc(&g_freecam.programmable_draws);
        if (!g_freecam.renderer_settings.enabled || g_freecam.draw_active) {
            return;
        }
        const bool pose_active =
            !freecam_pose_is_identity(&g_freecam.renderer_pose);
        if (!pose_active && !g_freecam.renderer_settings.fov_override) {
            return;
        }

        /* Programmable shaders retain the Milestone 3.1 post-VSH compatibility
         * path in both camera modes.  The reconstructed-view mode is currently
         * a fixed-function preference because that is where the guest exposes
         * MMAT/CMAT state that can be split safely without game-specific VSH
         * knowledge. */
        (void)freecam_transform_programmable_draw(pg);
        return;
    } else {
        return;
    }

    if (!g_freecam.renderer_settings.enabled || g_freecam.draw_active) {
        return;
    }

    const bool pose_active = !freecam_pose_is_identity(&g_freecam.renderer_pose);
    if (!pose_active && !g_freecam.renderer_settings.fov_override) {
        return;
    }

    static const unsigned int bases[FREECAM_SAVED_MATRICES] = {
        NV_IGRAPH_XF_XFCTX_CMAT0,
        NV_IGRAPH_XF_XFCTX_MMAT0,
        NV_IGRAPH_XF_XFCTX_MMAT1,
        NV_IGRAPH_XF_XFCTX_MMAT2,
        NV_IGRAPH_XF_XFCTX_MMAT3,
        NV_IGRAPH_XF_XFCTX_IMMAT0,
        NV_IGRAPH_XF_XFCTX_IMMAT1,
        NV_IGRAPH_XF_XFCTX_IMMAT2,
        NV_IGRAPH_XF_XFCTX_IMMAT3,
    };
    for (int i = 0; i < FREECAM_SAVED_MATRICES; ++i) {
        freecam_save_matrix(pg, i, bases[i]);
    }
    g_freecam.draw_active = true;
    g_freecam.fixed_saved_active = true;

    float view_delta[4][4], inverse_view_delta[4][4];
    freecam_build_view_delta(&g_freecam.renderer_pose, view_delta);
    if (pose_active && !freecam_matrix_inverse(view_delta, inverse_view_delta)) {
        freecam_restore_saved(pg);
        qatomic_inc(&g_freecam.transform_failures);
        return;
    }

    const unsigned int skinning = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_D), NV_PGRAPH_CSV0_D_SKIN);
    const bool reconstructed_requested =
        g_freecam.renderer_settings.render_mode ==
        XEMU_FREECAM_RENDER_RECONSTRUCTED_VIEW;

    /* Capture the original MMAT0 before the common lighting/texgen update.
     * Reconstructed-view mode uses this to split CMAT into model-view and an
     * effective projection for unskinned draws. */
    float original_model_view0[4][4];
    freecam_matrix_read(pg, NV_IGRAPH_XF_XFCTX_MMAT0, original_model_view0);

    float projection[4][4], composite[4][4], new_composite[4][4];
    float recovered_projection[4][4];
    bool recovered_projection_valid = false;
    freecam_matrix_read(pg, NV_IGRAPH_XF_XFCTX_PMAT0, projection);
    freecam_matrix_read(pg, NV_IGRAPH_XF_XFCTX_CMAT0, composite);
    memcpy(new_composite, composite, sizeof(new_composite));

    enum {
        FIXED_PATH_NONE = 0,
        FIXED_PATH_EXACT,
        FIXED_PATH_RECONSTRUCTED,
        FIXED_PATH_PROJECTIVE,
    } fixed_path = FIXED_PATH_NONE;

    /* Reconstructed-view mode now prefers a CMAT-only camera split. Max Payne
     * demonstrated that PMAT/MMAT0 can both be unusable while CMAT itself still
     * carries a normal perspective camera. Factor that first; retain the M4
     * MMAT0+CMAT recovery as a secondary path for titles where it is valid. */
    bool reconstructed_from_composite = false;
    bool reconstructed_from_mmat = false;
    if (reconstructed_requested && skinning == 0) {
        float pre_projection[4][4];
        if (freecam_composite_has_perspective_w(composite)) {
            qatomic_inc(&g_freecam.fixed_composite_inference_attempts);
            recovered_projection_valid = freecam_factor_composite_camera(
                composite, pre_projection, recovered_projection);
            if (!recovered_projection_valid) {
                qatomic_inc(&g_freecam.fixed_composite_inference_rejected);
            }
        }
        if (recovered_projection_valid) {
            reconstructed_from_composite = true;
            if (pose_active) {
                float moved_pre_projection[4][4];
                freecam_matrix_mul(pre_projection, view_delta,
                                   moved_pre_projection);
                freecam_matrix_mul(moved_pre_projection, recovered_projection,
                                   new_composite);
                fixed_path = FIXED_PATH_RECONSTRUCTED;
            }
        } else {
            recovered_projection_valid = freecam_recover_fixed_projection(
                original_model_view0, composite, recovered_projection);
            if (recovered_projection_valid) {
                reconstructed_from_mmat = true;
                if (pose_active) {
                    float moved_model_view[4][4];
                    freecam_matrix_mul(original_model_view0, view_delta,
                                       moved_model_view);
                    freecam_matrix_mul(moved_model_view, recovered_projection,
                                       new_composite);
                    fixed_path = FIXED_PATH_RECONSTRUCTED;
                }
            }
        }

        if (recovered_projection_valid &&
            g_freecam.renderer_settings.fov_override) {
            const float fov_scale = freecam_fov_scale(
                &g_freecam.renderer_settings, recovered_projection);
            if (fabsf(fov_scale - 1.0f) > 1.0e-6f) {
                freecam_scale_clip_xy(new_composite, fov_scale);
                fixed_path = FIXED_PATH_RECONSTRUCTED;
            } else if (!pose_active) {
                fixed_path = FIXED_PATH_NONE;
            }
        }
    }

    /* Keep fixed-function lighting, fog, eye-linear texgen and material
     * observation in the same view basis as the rendered geometry. Skinned
     * positions also consume these MMATs directly. */
    if (pose_active) {
        for (int i = 0; i < 4; ++i) {
            float model_view[4][4], modified[4][4];
            unsigned int base = NV_IGRAPH_XF_XFCTX_MMAT0 + i * 8;
            freecam_matrix_read(pg, base, model_view);
            freecam_matrix_mul(model_view, view_delta, modified);
            freecam_matrix_write(pg, base, modified);

            float inverse_model_view[4][4], modified_inverse[4][4];
            base = NV_IGRAPH_XF_XFCTX_IMMAT0 + i * 8;
            freecam_matrix_read(pg, base, inverse_model_view);
            /* If M' = M * D, then inverse(M') = inverse(D) * inverse(M).
             * The older freecam path multiplied the inverse matrix by D on
             * the wrong side, which could make fixed-function lighting and
             * normal transforms diverge from the moved geometry. */
            freecam_matrix_mul(inverse_view_delta, inverse_model_view,
                               modified_inverse);
            freecam_matrix_write(pg, base, modified_inverse);
        }
    }

    /* Skinned fixed-function positions are already moved through MMAT0-3.
     * CMAT is the projection-stage transform in that path, so no second camera
     * delta belongs in CMAT. */
    if (skinning != 0 && pose_active) {
        fixed_path = reconstructed_requested ? FIXED_PATH_RECONSTRUCTED
                                             : FIXED_PATH_EXACT;
    }

    /* Preserve the original Milestone 3 exact PMAT-factorization path when the
     * user selects Projective compatibility mode. This intentionally keeps the
     * existing mode bit-for-bit in spirit instead of replacing it globally. */
    if (!reconstructed_requested && skinning == 0 && pose_active) {
        float inv_projection[4][4], pre_projection[4][4], tmp[4][4];
        if (freecam_matrix_inverse(projection, inv_projection)) {
            freecam_matrix_mul(composite, inv_projection, pre_projection);
            freecam_matrix_mul(pre_projection, view_delta, tmp);
            freecam_matrix_mul(tmp, projection, new_composite);
            fixed_path = FIXED_PATH_EXACT;
        }
    }

    /* FOV override on exact/skinned paths continues to use the guest PMAT.
     * Reconstructed unskinned draws already used recovered_projection above. */
    if (fixed_path == FIXED_PATH_EXACT &&
        g_freecam.renderer_settings.fov_override) {
        const float fov_scale = freecam_fov_scale(
            &g_freecam.renderer_settings, projection);
        if (fabsf(fov_scale - 1.0f) > 1.0e-6f) {
            freecam_scale_clip_xy(new_composite, fov_scale);
        }
    }

    /* In Reconstructed View, obvious non-perspective fixed-function draws are
     * normally HUD/overlay/2D work. The old projective fallback visibly drags
     * and repeats those layers with the 3D world, so leave them in the guest's
     * original screen space instead. Perspective-looking draws still retain
     * the M3.1 compatibility fallback if both true-view recovery routes fail. */
    const bool needs_transform =
        pose_active || g_freecam.renderer_settings.fov_override;
    if (fixed_path == FIXED_PATH_NONE && needs_transform &&
        reconstructed_requested &&
        !freecam_composite_has_perspective_w(composite)) {
        freecam_restore_saved(pg);
        qatomic_inc(&g_freecam.fixed_nonperspective_passthrough_draws);
        return;
    }

    /* Any remaining 3D case that cannot be expressed safely in true/recovered
     * view space falls back to the existing projective transform. */
    if (fixed_path == FIXED_PATH_NONE && needs_transform) {
        float raw_post[4][4], fallback_composite[4][4];
        if (freecam_build_fixed_raw_post_transform(
                pg, &g_freecam.renderer_settings, &g_freecam.renderer_pose,
                raw_post)) {
            freecam_matrix_mul(composite, raw_post, fallback_composite);
            memcpy(new_composite, fallback_composite, sizeof(new_composite));
            fixed_path = FIXED_PATH_PROJECTIVE;
            if (reconstructed_requested) {
                qatomic_inc(&g_freecam.fixed_reconstructed_fallback_draws);
            }
        }
    }

    if (fixed_path == FIXED_PATH_NONE) {
        freecam_restore_saved(pg);
        qatomic_inc(&g_freecam.transform_failures);
        return;
    }

    freecam_matrix_write(pg, NV_IGRAPH_XF_XFCTX_CMAT0, new_composite);
    switch (fixed_path) {
    case FIXED_PATH_EXACT:
        qatomic_inc(&g_freecam.fixed_exact_transformed_draws);
        break;
    case FIXED_PATH_RECONSTRUCTED:
        qatomic_inc(&g_freecam.fixed_reconstructed_transformed_draws);
        if (reconstructed_from_composite) {
            qatomic_inc(&g_freecam.fixed_composite_inferred_transformed_draws);
        } else if (reconstructed_from_mmat) {
            qatomic_inc(&g_freecam.fixed_mmat_reconstructed_transformed_draws);
        }
        break;
    case FIXED_PATH_PROJECTIVE:
        qatomic_inc(&g_freecam.fixed_projective_transformed_draws);
        break;
    default:
        break;
    }
    qatomic_inc(&g_freecam.transformed_draws);
    qatomic_set(&g_freecam.draw_transform_active, 1);
}

void xemu_freecam_renderer_draw_end(NV2AState *d)
{
    if (!d) {
        return;
    }
    freecam_init_once();
    freecam_restore_saved(&d->pgraph);
}

void xemu_freecam_renderer_abort_draw(NV2AState *d)
{
    xemu_freecam_renderer_draw_end(d);
}

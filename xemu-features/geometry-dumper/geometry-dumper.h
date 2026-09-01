/*
 * xemu custom fork - NV2A geometry dumper public boundary
 *
 * Feature-owned integration only. The implementation wraps the active PGRAPH
 * renderer from an existing xemu-features renderer-init hook; no upstream NV2A
 * draw source needs to be modified.
 */
#ifndef XEMU_FEATURES_GEOMETRY_DUMPER_H
#define XEMU_FEATURES_GEOMETRY_DUMPER_H

#include "config-host.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XemuGeometryCaptureMode {
    XEMU_GEOMETRY_CAPTURE_IDLE = 0,
    XEMU_GEOMETRY_CAPTURE_NEXT_DRAW,
    XEMU_GEOMETRY_CAPTURE_NEXT_FRAME_WAIT,
    XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE,
} XemuGeometryCaptureMode;

typedef struct XemuGeometryCaptureOptions {
    /* Used by frame capture. Values < 1 are clamped to 1. */
    uint32_t frame_count;

    /* Temporarily clears NV2A rasterizer culling only for captured draws. The
     * raw geometry dump is pre-rasterizer and therefore already contains faces
     * regardless of the guest cull setting; this option is for the rendered
     * image while a capture is active. */
    bool disable_backface_culling;

    /* Also create geometry_placed.obj for fixed-function draws by applying the
     * exact draw-time NV2A model-view/skinning matrices. This is camera/view
     * space, which preserves per-draw scene placement relative to the camera.
     * Arbitrary programmable vertex shaders cannot be generically converted to
     * world space and remain available in the raw export plus metadata. */
    bool export_placed_geometry;

    /* Uniform multiplier applied only to exported OBJ positions. 1.0 preserves
     * the current/native dump size. Raw CSV/JSON metadata remains unscaled. */
    float export_scale;

    /* Export active base-level NV2A textures. V5 also reconstructs post-VSH
     * texture coordinates and only creates OBJ/MTL associations for direct
     * 2D sampling stages that OBJ can represent faithfully. */
    bool dump_textures;
} XemuGeometryCaptureOptions;

typedef struct XemuGeometryDumperStatus {
    bool renderer_hooked;
    XemuGeometryCaptureMode mode;
    uint64_t capture_serial;
    uint64_t draws_captured;
    uint64_t vertices_captured;
    uint64_t primitives_captured;
    uint64_t placed_draws_captured;
    uint64_t placed_draws_unsupported;
    uint32_t frames_requested;
    uint32_t frames_completed;
    uint32_t active_frame_index;
    bool disable_backface_culling;
    bool export_placed_geometry;
    float export_scale;
    bool dump_textures;
    uint64_t textures_referenced;
    uint64_t textures_dumped;
    uint64_t texture_dump_failures;
    int32_t frame_time;
    char output_path[1024];
    char last_error[512];
} XemuGeometryDumperStatus;

#ifdef CONFIG_XEMU_FEATURE_TEXTURE_PACKS
/* Called from the existing feature-owned NV2A renderer-init integration. */
void xemu_geometry_dumper_renderer_ready(void);

/* Compatibility helpers using V1 behavior/defaults. */
bool xemu_geometry_dumper_capture_next_draw(const char *output_root);
bool xemu_geometry_dumper_capture_next_frame(const char *output_root);

/* V5 control API. glTF 2.0 is emitted for every capture. */
bool xemu_geometry_dumper_capture_next_draw_ex(
    const char *output_root, const XemuGeometryCaptureOptions *options);
bool xemu_geometry_dumper_capture_frames(
    const char *output_root, const XemuGeometryCaptureOptions *options);
void xemu_geometry_dumper_cancel_capture(void);
void xemu_geometry_dumper_get_status(XemuGeometryDumperStatus *status);
#else
static inline void xemu_geometry_dumper_renderer_ready(void) {}
static inline bool xemu_geometry_dumper_capture_next_draw(const char *output_root)
{ (void)output_root; return false; }
static inline bool xemu_geometry_dumper_capture_next_frame(const char *output_root)
{ (void)output_root; return false; }
static inline bool xemu_geometry_dumper_capture_next_draw_ex(
    const char *output_root, const XemuGeometryCaptureOptions *options)
{ (void)output_root; (void)options; return false; }
static inline bool xemu_geometry_dumper_capture_frames(
    const char *output_root, const XemuGeometryCaptureOptions *options)
{ (void)output_root; (void)options; return false; }
static inline void xemu_geometry_dumper_cancel_capture(void) {}
static inline void xemu_geometry_dumper_get_status(XemuGeometryDumperStatus *status)
{ if (status) { memset(status, 0, sizeof(*status)); } }
#endif

#ifdef __cplusplus
}
#endif

#endif

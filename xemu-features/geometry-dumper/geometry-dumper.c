/*
 * xemu custom fork - NV2A geometry dumper
 *
 * Captures raw, pre-vertex-shader NV2A draw input.  The export is deliberately
 * faithful to the guest-visible vertex streams: no coordinate-system repair,
 * smoothing, normal generation, welding, or topology beautification is done.
 *
 * Build integration note:
 * This file is included by the already feature-owned texture-packs translation
 * unit so the custom fork does not need to edit upstream PGRAPH Meson/source
 * files merely to install a draw observer.  If this feature is ever upstreamed,
 * it can become its own normal Meson source without changing the API below.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/vsh_regs.h"
#include "hw/xbox/nv2a/pgraph/psh_regs.h"
#include "nv2a_vsh_emulator.h"
#include "qemu/fast-hash.h"
#include "ui/xemu-settings.h"
#include "xemu-xbe.h"
#include "xemu-features/geometry-dumper/geometry-dumper.h"
#include "xemu-features/freecam/freecam.h"
#include "xemu-features/texture-packs/texture-packs.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>

#define GEOM_PATH_CAP 1024
#define GEOM_ERROR_CAP 512

typedef struct GeometryVertex {
    /* Decoded guest-visible vertex shader inputs v0-v15. Keeping all 16 is
     * important: programmable shaders are free to source texture coordinates
     * from any input register, not merely v9/TEXCOORD0. */
    float shader_input[NV2A_VERTEXSHADER_ATTRIBUTES][4];

    /* Convenience copies used by the raw/fixed-function exporters. */
    float position[4];
    float weight[4];
    float normal[4];
    float tex0[4];
    float diffuse[4];

    /* Post-vertex-shader oT0-oT3. These are evaluated only while capturing.
     * They are not substituted into the raw CSV evidence. */
    float post_vsh_texcoord[NV2A_MAX_TEXTURES][4];
    bool post_vsh_texcoord_valid[NV2A_MAX_TEXTURES];
    float post_vsh_position[4];
    bool post_vsh_position_valid;

    /* Linearized position used by the live material observer. Fixed-function
     * draws store transformed eye-space xyz; programmable draws store
     * (oPos.x, oPos.y, oPos.w), which preserves a stable linear projective
     * basis without a perspective divide. Compute this once per vertex and
     * reuse it for every triangle/material stage in the draw. */
    float material_position[3];
    bool material_position_valid;
    uint32_t source_index;
} GeometryVertex;

typedef struct GeometryUV {
    float u;
    float v;
    bool valid;
} GeometryUV;

typedef struct GeometrySegment {
    uint32_t first_vertex;
    uint32_t vertex_count;
} GeometrySegment;

typedef enum GeometrySourceKind {
    GEOM_SOURCE_NONE = 0,
    GEOM_SOURCE_DRAW_ARRAYS,
    GEOM_SOURCE_INLINE_ELEMENTS,
    GEOM_SOURCE_INLINE_BUFFER,
    GEOM_SOURCE_INLINE_ARRAY,
} GeometrySourceKind;


typedef struct GeometryGltfPrimitive {
    uint32_t frame_index;
    uint64_t draw_id;
    uint32_t segment_index;
    uint32_t source_kind;
    uint32_t xbox_primitive_mode;
    uint32_t gltf_mode;
    uint32_t vertex_count;
    uint32_t index_count;
    uint64_t position_offset;
    uint64_t normal_offset;
    uint64_t uv_offset;
    uint64_t index_offset;
    bool has_normal;
    bool has_uv;
    int material_texture_slot;
    int gltf_mag_filter;
    int gltf_min_filter;
    int gltf_wrap_s;
    int gltf_wrap_t;
    bool sampler_border_approx;
    bool blend_enabled;
    bool alpha_test_enabled;
    unsigned int alpha_ref;
    unsigned int alpha_func;
    char texture_file[256];
    float position_min[3];
    float position_max[3];
    int position_accessor;
    int normal_accessor;
    int uv_accessor;
    int index_accessor;
    int material_index;
    int mesh_index;
} GeometryGltfPrimitive;

typedef struct GeometryGltfExport {
    bool enabled;
    bool placed;
    bool native_basis;
    FILE *bin;
    GArray *primitives; /* GeometryGltfPrimitive */
    uint64_t byte_length;
    char json_path[GEOM_PATH_CAP];
    char bin_path[GEOM_PATH_CAP];
    char bin_uri[128];
} GeometryGltfExport;

typedef struct GeometryCaptureState {
    QemuMutex lock;
    bool initialized;
    bool renderer_hooked;
    int flip_stall_seen;

    int mode; /* XemuGeometryCaptureMode; atomically observed on draw thread. */
    uint64_t capture_serial;
    uint64_t draw_serial;
    uint64_t draws_captured;
    uint64_t vertices_captured;
    uint64_t primitives_captured;
    uint64_t placed_draws_captured;
    uint64_t placed_draws_unsupported;
    uint32_t frames_requested;
    uint32_t frames_completed;
    uint32_t active_frame_index;
    int32_t frame_time;
    int32_t wait_frame_time;
    bool disable_backface_culling;
    bool export_placed_geometry;
    float export_scale;
    bool dump_textures;
    bool current_frame_started;
    uint64_t textures_referenced;
    uint64_t textures_dumped;
    uint64_t texture_dump_failures;

    char request_root[GEOM_PATH_CAP];
    uint32_t request_title_id;
    char output_path[GEOM_PATH_CAP];
    char last_error[GEOM_ERROR_CAP];

    FILE *obj;
    FILE *textured_obj;
    FILE *placed_obj;
    FILE *jsonl;
    FILE *csv;
    FILE *mtl;
    FILE *texture_manifest;
    GHashTable *dumped_texture_keys;
    GHashTable *failed_texture_keys;
    GeometryGltfExport gltf_primary;
    GeometryGltfExport gltf_raw;
    uint64_t obj_vertex_base;
    uint64_t textured_obj_vertex_base;
    uint64_t placed_obj_vertex_base;
} GeometryCaptureState;

static GeometryCaptureState g_geometry;
static gsize g_geometry_init_once;

static PGRAPHRenderer g_geometry_renderer;
static void (*g_geometry_original_draw_begin)(NV2AState *d);
static void (*g_geometry_original_draw_end)(NV2AState *d);
static void (*g_geometry_original_flush_draw)(NV2AState *d);
static void (*g_geometry_original_flip_stall)(NV2AState *d);

static void geometry_capture_draw_begin(NV2AState *d);
static void geometry_capture_draw_end(NV2AState *d);
static void geometry_capture_flush_draw(NV2AState *d);
static void geometry_capture_flip_stall(NV2AState *d);

static void geometry_init_once(void)
{
    if (g_once_init_enter(&g_geometry_init_once)) {
        memset(&g_geometry, 0, sizeof(g_geometry));
        qemu_mutex_init(&g_geometry.lock);
        g_geometry.initialized = true;
        g_geometry.export_scale = 1.0f;
        qatomic_set(&g_geometry.mode, XEMU_GEOMETRY_CAPTURE_IDLE);
        g_once_init_leave(&g_geometry_init_once, 1);
    }
}

static const char *geometry_primitive_name(uint32_t primitive)
{
    switch (primitive) {
    case PRIM_TYPE_POINTS: return "points";
    case PRIM_TYPE_LINES: return "lines";
    case PRIM_TYPE_LINE_LOOP: return "line_loop";
    case PRIM_TYPE_LINE_STRIP: return "line_strip";
    case PRIM_TYPE_TRIANGLES: return "triangles";
    case PRIM_TYPE_TRIANGLE_STRIP: return "triangle_strip";
    case PRIM_TYPE_TRIANGLE_FAN: return "triangle_fan";
    case PRIM_TYPE_QUADS: return "quads";
    case PRIM_TYPE_QUAD_STRIP: return "quad_strip";
    case PRIM_TYPE_POLYGON: return "polygon";
    default: return "invalid";
    }
}

static const char *geometry_source_name(GeometrySourceKind source)
{
    switch (source) {
    case GEOM_SOURCE_DRAW_ARRAYS: return "draw_arrays";
    case GEOM_SOURCE_INLINE_ELEMENTS: return "inline_elements";
    case GEOM_SOURCE_INLINE_BUFFER: return "inline_buffer";
    case GEOM_SOURCE_INLINE_ARRAY: return "inline_array";
    default: return "none";
    }
}

static void geometry_set_error_locked(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_geometry.last_error, sizeof(g_geometry.last_error), fmt, ap);
    va_end(ap);
}

static void geometry_gltf_finalize_exports_locked(void);

static void geometry_close_files_locked(void)
{
    if (g_geometry.obj) {
        fflush(g_geometry.obj);
        fclose(g_geometry.obj);
        g_geometry.obj = NULL;
    }
    if (g_geometry.textured_obj) {
        fflush(g_geometry.textured_obj);
        fclose(g_geometry.textured_obj);
        g_geometry.textured_obj = NULL;
    }
    if (g_geometry.placed_obj) {
        fflush(g_geometry.placed_obj);
        fclose(g_geometry.placed_obj);
        g_geometry.placed_obj = NULL;
    }
    if (g_geometry.jsonl) {
        fflush(g_geometry.jsonl);
        fclose(g_geometry.jsonl);
        g_geometry.jsonl = NULL;
    }
    if (g_geometry.csv) {
        fflush(g_geometry.csv);
        fclose(g_geometry.csv);
        g_geometry.csv = NULL;
    }
    if (g_geometry.mtl) {
        fflush(g_geometry.mtl);
        fclose(g_geometry.mtl);
        g_geometry.mtl = NULL;
    }
    if (g_geometry.texture_manifest) {
        fflush(g_geometry.texture_manifest);
        fclose(g_geometry.texture_manifest);
        g_geometry.texture_manifest = NULL;
    }
    if (g_geometry.gltf_primary.bin) {
        fflush(g_geometry.gltf_primary.bin);
        fclose(g_geometry.gltf_primary.bin);
        g_geometry.gltf_primary.bin = NULL;
    }
    if (g_geometry.gltf_primary.primitives) {
        g_array_free(g_geometry.gltf_primary.primitives, TRUE);
        g_geometry.gltf_primary.primitives = NULL;
    }
    g_geometry.gltf_primary.enabled = false;
    if (g_geometry.gltf_raw.bin) {
        fflush(g_geometry.gltf_raw.bin);
        fclose(g_geometry.gltf_raw.bin);
        g_geometry.gltf_raw.bin = NULL;
    }
    if (g_geometry.gltf_raw.primitives) {
        g_array_free(g_geometry.gltf_raw.primitives, TRUE);
        g_geometry.gltf_raw.primitives = NULL;
    }
    g_geometry.gltf_raw.enabled = false;
    if (g_geometry.dumped_texture_keys) {
        g_hash_table_destroy(g_geometry.dumped_texture_keys);
        g_geometry.dumped_texture_keys = NULL;
    }
    if (g_geometry.failed_texture_keys) {
        g_hash_table_destroy(g_geometry.failed_texture_keys);
        g_geometry.failed_texture_keys = NULL;
    }
}

static void geometry_finalize_capture_locked(void)
{
    geometry_gltf_finalize_exports_locked();
    if (g_geometry.obj) {
        fprintf(g_geometry.obj,
                "# capture complete: draws=%" PRIu64 " vertices=%" PRIu64
                " primitives=%" PRIu64 " frames=%u/%u textures=%" PRIu64
                " texture_failures=%" PRIu64 "\n",
                g_geometry.draws_captured, g_geometry.vertices_captured,
                g_geometry.primitives_captured, g_geometry.frames_completed,
                g_geometry.frames_requested, g_geometry.textures_dumped,
                g_geometry.texture_dump_failures);
    }
    if (g_geometry.textured_obj) {
        fprintf(g_geometry.textured_obj,
                "# capture complete: draws=%" PRIu64
                " frames=%u/%u\n",
                g_geometry.draws_captured, g_geometry.frames_completed,
                g_geometry.frames_requested);
    }
    if (g_geometry.placed_obj) {
        fprintf(g_geometry.placed_obj,
                "# capture complete: placed_draws=%" PRIu64
                " unsupported_draws=%" PRIu64 " frames=%u/%u\n",
                g_geometry.placed_draws_captured,
                g_geometry.placed_draws_unsupported,
                g_geometry.frames_completed, g_geometry.frames_requested);
    }
    geometry_close_files_locked();
    qatomic_set(&g_geometry.mode, XEMU_GEOMETRY_CAPTURE_IDLE);
}

static bool geometry_begin_capture_locked(int32_t frame_time)
{
    char title_hex[9];
    snprintf(title_hex, sizeof(title_hex), "%08X", g_geometry.request_title_id);

    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *stamp = now ? g_date_time_format(now, "%Y-%m-%d_%H-%M-%S")
                                 : g_strdup("unknown-time");

    const char *root = g_geometry.request_root;
    if (!root[0]) {
        geometry_set_error_locked("Geometry output root was not prepared.");
        return false;
    }

    g_geometry.capture_serial++;
    g_autofree char *capture_name = g_strdup_printf(
        "capture_%s_frame_%d_%06" PRIu64, stamp ? stamp : "unknown-time",
        frame_time, g_geometry.capture_serial);
    g_autofree char *path = g_build_filename(root, title_hex, capture_name, NULL);

    if (g_mkdir_with_parents(path, 0755) != 0) {
        geometry_set_error_locked("Could not create geometry dump directory: %s",
                                  path);
        return false;
    }

    g_autofree char *obj_path = g_build_filename(path, "geometry.obj", NULL);
    g_autofree char *textured_obj_path =
        g_build_filename(path, "geometry_textured.obj", NULL);
    g_autofree char *placed_obj_path =
        g_build_filename(path, "geometry_placed.obj", NULL);
    g_autofree char *jsonl_path = g_build_filename(path, "draws.jsonl", NULL);
    g_autofree char *csv_path = g_build_filename(path, "vertices.csv", NULL);
    g_autofree char *mtl_path = g_build_filename(path, "materials.mtl", NULL);
    g_autofree char *texture_manifest_path =
        g_build_filename(path, "texture_manifest.jsonl", NULL);
    g_autofree char *textures_path = g_build_filename(path, "textures", NULL);
    g_autofree char *gltf_path = g_build_filename(path, "geometry.gltf", NULL);
    g_autofree char *gltf_bin_path = g_build_filename(path, "geometry.bin", NULL);
    g_autofree char *raw_gltf_path = g_build_filename(path, "geometry_raw.gltf", NULL);
    g_autofree char *raw_gltf_bin_path = g_build_filename(path, "geometry_raw.bin", NULL);

    g_geometry.obj = g_fopen(obj_path, "wb");
    if (!g_geometry.obj) {
        geometry_set_error_locked("Could not create %s", obj_path);
        return false;
    }
    if (g_geometry.dump_textures) {
        g_geometry.textured_obj = g_fopen(textured_obj_path, "wb");
        if (!g_geometry.textured_obj) {
            geometry_set_error_locked("Could not create %s", textured_obj_path);
            geometry_close_files_locked();
            return false;
        }
    }
    if (g_geometry.export_placed_geometry) {
        g_geometry.placed_obj = g_fopen(placed_obj_path, "wb");
        if (!g_geometry.placed_obj) {
            geometry_set_error_locked("Could not create %s", placed_obj_path);
            geometry_close_files_locked();
            return false;
        }
    }
    g_geometry.jsonl = g_fopen(jsonl_path, "wb");
    if (!g_geometry.jsonl) {
        geometry_set_error_locked("Could not create %s", jsonl_path);
        geometry_close_files_locked();
        return false;
    }
    g_geometry.csv = g_fopen(csv_path, "wb");
    if (!g_geometry.csv) {
        geometry_set_error_locked("Could not create %s", csv_path);
        geometry_close_files_locked();
        return false;
    }
    if (g_geometry.dump_textures) {
        if (g_mkdir_with_parents(textures_path, 0755) != 0) {
            geometry_set_error_locked("Could not create texture dump directory: %s",
                                      textures_path);
            geometry_close_files_locked();
            return false;
        }
        g_geometry.mtl = g_fopen(mtl_path, "wb");
        g_geometry.texture_manifest = g_fopen(texture_manifest_path, "wb");
        if (!g_geometry.mtl || !g_geometry.texture_manifest) {
            geometry_set_error_locked(
                "Could not create geometry texture material/manifest files.");
            geometry_close_files_locked();
            return false;
        }
        g_geometry.dumped_texture_keys =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_geometry.failed_texture_keys =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }

    memset(&g_geometry.gltf_primary, 0, sizeof(g_geometry.gltf_primary));
    memset(&g_geometry.gltf_raw, 0, sizeof(g_geometry.gltf_raw));
    g_geometry.gltf_primary.enabled = true;
    g_geometry.gltf_primary.placed = g_geometry.export_placed_geometry;
    /* Primary interchange output is DCC-facing right-handed geometry. Raw
     * evidence remains available in geometry.obj/CSV and geometry_raw.gltf. */
    g_geometry.gltf_primary.native_basis = false;
    g_geometry.gltf_primary.bin = g_fopen(gltf_bin_path, "wb");
    g_geometry.gltf_primary.primitives =
        g_array_new(FALSE, FALSE, sizeof(GeometryGltfPrimitive));
    g_strlcpy(g_geometry.gltf_primary.json_path, gltf_path,
              sizeof(g_geometry.gltf_primary.json_path));
    g_strlcpy(g_geometry.gltf_primary.bin_path, gltf_bin_path,
              sizeof(g_geometry.gltf_primary.bin_path));
    g_strlcpy(g_geometry.gltf_primary.bin_uri, "geometry.bin",
              sizeof(g_geometry.gltf_primary.bin_uri));
    if (!g_geometry.gltf_primary.bin || !g_geometry.gltf_primary.primitives) {
        geometry_set_error_locked("Could not create primary glTF stream.");
        geometry_close_files_locked();
        return false;
    }
    if (g_geometry.export_placed_geometry) {
        g_geometry.gltf_raw.enabled = true;
        g_geometry.gltf_raw.placed = false;
        g_geometry.gltf_raw.native_basis = true;
        g_geometry.gltf_raw.bin = g_fopen(raw_gltf_bin_path, "wb");
        g_geometry.gltf_raw.primitives =
            g_array_new(FALSE, FALSE, sizeof(GeometryGltfPrimitive));
        g_strlcpy(g_geometry.gltf_raw.json_path, raw_gltf_path,
                  sizeof(g_geometry.gltf_raw.json_path));
        g_strlcpy(g_geometry.gltf_raw.bin_path, raw_gltf_bin_path,
                  sizeof(g_geometry.gltf_raw.bin_path));
        g_strlcpy(g_geometry.gltf_raw.bin_uri, "geometry_raw.bin",
                  sizeof(g_geometry.gltf_raw.bin_uri));
        if (!g_geometry.gltf_raw.bin || !g_geometry.gltf_raw.primitives) {
            geometry_set_error_locked("Could not create raw glTF companion stream.");
            geometry_close_files_locked();
            return false;
        }
    }

    g_strlcpy(g_geometry.output_path, path, sizeof(g_geometry.output_path));
    g_geometry.last_error[0] = '\0';
    g_geometry.frame_time = frame_time;
    g_geometry.draw_serial = 0;
    g_geometry.draws_captured = 0;
    g_geometry.vertices_captured = 0;
    g_geometry.primitives_captured = 0;
    g_geometry.placed_draws_captured = 0;
    g_geometry.placed_draws_unsupported = 0;
    g_geometry.textures_referenced = 0;
    g_geometry.textures_dumped = 0;
    g_geometry.texture_dump_failures = 0;
    g_geometry.frames_completed = 0;
    g_geometry.active_frame_index = 0;
    g_geometry.current_frame_started = false;
    g_geometry.obj_vertex_base = 0;
    g_geometry.textured_obj_vertex_base = 0;
    g_geometry.placed_obj_vertex_base = 0;

    fprintf(g_geometry.obj,
            "# xemu custom fork NV2A geometry dump v5\n"
            "# RAW pre-vertex-shader guest geometry and RAW TEXCOORD0.\n"
            "# This file intentionally does not apply materials because Xbox\n"
            "# texture coordinates may be generated/transformed by the vertex shader.\n"
            "# OBJ position export_scale=%.9g; JSON/CSV remain native/unscaled\n"
            "# title_id=%s start_frame_time=%d requested_frames=%u\n\n",
            g_geometry.export_scale, title_hex, frame_time,
            g_geometry.frames_requested);
    if (g_geometry.textured_obj) {
        fprintf(g_geometry.textured_obj,
                "# xemu custom fork NV2A geometry dump v5\n"
                "# raw positions + reconstructed post-vertex-shader texture coordinates\n"
                "# only directly projectable 2D texture stages are auto-mapped\n"
                "# OBJ position export_scale=%.9g\n"
                "# title_id=%s start_frame_time=%d requested_frames=%u\n"
                "mtllib materials.mtl\n\n",
                g_geometry.export_scale, title_hex, frame_time,
                g_geometry.frames_requested);
    }
    if (g_geometry.placed_obj) {
        fprintf(g_geometry.placed_obj,
                "# xemu custom fork NV2A geometry dump v5\n"
                "# fixed-function draw-time model-view/skinning placement\n"
                "# coordinates are camera/view space, not guaranteed world space\n"
                "# programmable vertex-shader draws are intentionally omitted\n"
                "# OBJ position export_scale=%.9g; JSON/CSV remain native/unscaled\n"
                "# title_id=%s start_frame_time=%d requested_frames=%u\n",
                g_geometry.export_scale, title_hex, frame_time,
                g_geometry.frames_requested);
        if (g_geometry.dump_textures) {
            fprintf(g_geometry.placed_obj, "mtllib materials.mtl\n");
        }
        fputc('\n', g_geometry.placed_obj);
    }
    fprintf(g_geometry.jsonl,
            "{\"type\":\"capture\",\"title_id\":\"%s\","
            "\"frame_time\":%d,\"format_version\":5,"
            "\"frames_requested\":%u,"
            "\"disable_backface_culling\":%s,"
            "\"export_placed_geometry\":%s,"
            "\"export_scale\":%.9g,\"dump_textures\":%s,"
            "\"gltf_primary\":\"geometry.gltf\","
            "\"gltf_raw\":%s}\n",
            title_hex, frame_time, g_geometry.frames_requested,
            g_geometry.disable_backface_culling ? "true" : "false",
            g_geometry.export_placed_geometry ? "true" : "false",
            g_geometry.export_scale,
            g_geometry.dump_textures ? "true" : "false",
            g_geometry.export_placed_geometry ? "\"geometry_raw.gltf\"" : "null");
    fprintf(g_geometry.csv,
            "frame_index,frame_time,draw,segment,local_vertex,source_index,"
            "px,py,pz,pw,weight_x,weight_y,weight_z,weight_w,"
            "nx,ny,nz,nw,u0,v0,s0,q0,"
            "diffuse_r,diffuse_g,diffuse_b,diffuse_a,"
            "placed_available,placed_px,placed_py,placed_pz,placed_pw\n");
    return true;
}

static void geometry_begin_frame_locked(int32_t frame_time)
{
    g_geometry.frame_time = frame_time;
    g_geometry.current_frame_started = true;
    if (g_geometry.jsonl) {
        fprintf(g_geometry.jsonl,
                "{\"type\":\"frame_begin\",\"frame_index\":%u,"
                "\"frame_time\":%d}\n",
                g_geometry.active_frame_index, frame_time);
    }
    if (g_geometry.obj) {
        fprintf(g_geometry.obj, "\n# --- frame %u (frame_time=%d) ---\n",
                g_geometry.active_frame_index, frame_time);
    }
    if (g_geometry.textured_obj) {
        fprintf(g_geometry.textured_obj,
                "\n# --- frame %u (frame_time=%d) ---\n",
                g_geometry.active_frame_index, frame_time);
    }
    if (g_geometry.placed_obj) {
        fprintf(g_geometry.placed_obj,
                "\n# --- frame %u (frame_time=%d) ---\n",
                g_geometry.active_frame_index, frame_time);
    }
}

static bool geometry_complete_frame_locked(void)
{
    if (g_geometry.jsonl) {
        fprintf(g_geometry.jsonl,
                "{\"type\":\"frame_end\",\"frame_index\":%u,"
                "\"frame_time\":%d}\n",
                g_geometry.active_frame_index, g_geometry.frame_time);
    }

    if (g_geometry.frames_completed < UINT32_MAX) {
        g_geometry.frames_completed++;
    }
    g_geometry.current_frame_started = false;

    if (g_geometry.frames_completed >= g_geometry.frames_requested) {
        geometry_finalize_capture_locked();
        return true;
    }

    g_geometry.active_frame_index = g_geometry.frames_completed;
    g_geometry.frame_time = INT32_MIN;
    return false;
}

static float geometry_snorm(int32_t v, float denom)
{
    float f = (float)v / denom;
    return MAX(-1.0f, f);
}

static bool geometry_decode_attr_bytes(const VertexAttribute *attr,
                                       const uint8_t *data,
                                       float out[4])
{
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;

    if (!attr || !data || attr->count > 4) {
        return false;
    }

    switch (attr->format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        /* Xemu binds this through GL_BGRA / the equivalent Vulkan swizzle.
         * Reproduce the value that the NV2A vertex shader actually sees. */
        if (attr->count == 4) {
            out[0] = (float)data[2] / 255.0f;
            out[1] = (float)data[1] / 255.0f;
            out[2] = (float)data[0] / 255.0f;
            out[3] = (float)data[3] / 255.0f;
            return true;
        }
        /* Unexpected non-BGRA count: preserve the generic normalized path. */
        /* fall through */
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        for (uint32_t i = 0; i < attr->count; ++i) {
            out[i] = (float)data[i] / 255.0f;
        }
        return true;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        for (uint32_t i = 0; i < attr->count; ++i) {
            int16_t v = (int16_t)lduw_le_p(data + i * sizeof(uint16_t));
            out[i] = geometry_snorm(v, 32767.0f);
        }
        return true;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        for (uint32_t i = 0; i < attr->count; ++i) {
            uint32_t u = ldl_le_p(data + i * sizeof(uint32_t));
            memcpy(&out[i], &u, sizeof(u));
        }
        return true;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        for (uint32_t i = 0; i < attr->count; ++i) {
            int16_t v = (int16_t)lduw_le_p(data + i * sizeof(uint16_t));
            out[i] = (float)v;
        }
        return true;

    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP: {
        int32_t val = (int32_t)ldl_le_p(data);
        int32_t x = val & 0x7ff;
        int32_t y = (val >> 11) & 0x7ff;
        int32_t z = (val >> 22) & 0x3ff;
        if (x & 0x400) x |= ~0x7ff;
        if (y & 0x400) y |= ~0x7ff;
        if (z & 0x200) z |= ~0x3ff;
        out[0] = geometry_snorm(x, 1023.0f);
        out[1] = geometry_snorm(y, 1023.0f);
        out[2] = geometry_snorm(z, 511.0f);
        return true;
    }
    default:
        return false;
    }
}

typedef struct GeometryAttrReader {
    const VertexAttribute *attr;
    const uint8_t *dma;
    hwaddr dma_len;
} GeometryAttrReader;

static void geometry_prepare_attr_reader(NV2AState *d, int slot,
                                         GeometryAttrReader *reader)
{
    PGRAPHState *pg = &d->pgraph;
    memset(reader, 0, sizeof(*reader));
    reader->attr = &pg->vertex_attributes[slot];
    if (reader->attr->count == 0) {
        return;
    }
    reader->dma = (const uint8_t *)nv_dma_map(
        d, reader->attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
        &reader->dma_len);
}

static bool geometry_read_attr(const GeometryAttrReader *reader,
                               uint32_t vertex_index, float out[4])
{
    const VertexAttribute *attr = reader->attr;
    if (!attr) {
        return false;
    }
    if (attr->count == 0) {
        memcpy(out, attr->inline_value, sizeof(float) * 4);
        return true;
    }
    if (!reader->dma || attr->offset >= reader->dma_len) {
        return false;
    }

    size_t elem_size = (size_t)attr->size * attr->count;
    uint64_t byte_offset = attr->offset;
    if (attr->stride != 0) {
        byte_offset += (uint64_t)vertex_index * attr->stride;
    }
    if (byte_offset > reader->dma_len ||
        elem_size > reader->dma_len - byte_offset) {
        return false;
    }
    return geometry_decode_attr_bytes(attr, reader->dma + byte_offset, out);
}

static void geometry_vertex_sync_convenience(GeometryVertex *v)
{
    memcpy(v->position, v->shader_input[NV2A_VERTEX_ATTR_POSITION],
           sizeof(v->position));
    memcpy(v->weight, v->shader_input[NV2A_VERTEX_ATTR_WEIGHT],
           sizeof(v->weight));
    memcpy(v->normal, v->shader_input[NV2A_VERTEX_ATTR_NORMAL],
           sizeof(v->normal));
    memcpy(v->tex0, v->shader_input[NV2A_VERTEX_ATTR_TEXTURE0],
           sizeof(v->tex0));
    memcpy(v->diffuse, v->shader_input[NV2A_VERTEX_ATTR_DIFFUSE],
           sizeof(v->diffuse));
}

static void geometry_read_vertex_dma(
    const GeometryAttrReader readers[NV2A_VERTEXSHADER_ATTRIBUTES],
    uint32_t source_index, uint16_t attr_mask, GeometryVertex *v)
{
    memset(v, 0, sizeof(*v));
    v->source_index = source_index;
    for (int slot = 0; slot < NV2A_VERTEXSHADER_ATTRIBUTES; ++slot) {
        if (!(attr_mask & (1u << slot))) {
            continue;
        }
        /* geometry_read_attr returns the inline/default value for disabled
         * arrays, matching how Xemu supplies uniform vertex attributes. */
        geometry_read_attr(&readers[slot], source_index,
                           v->shader_input[slot]);
    }
    geometry_vertex_sync_convenience(v);
}

static void geometry_read_vertex_inline_buffer(PGRAPHState *pg, uint32_t index,
                                               uint16_t attr_mask,
                                               GeometryVertex *v)
{
    memset(v, 0, sizeof(*v));
    v->source_index = index;

    for (int slot = 0; slot < NV2A_VERTEXSHADER_ATTRIBUTES; ++slot) {
        if (!(attr_mask & (1u << slot))) {
            continue;
        }
        VertexAttribute *attr = &pg->vertex_attributes[slot];
        if (attr->inline_buffer_populated && index < pg->inline_buffer_length) {
            memcpy(v->shader_input[slot], &attr->inline_buffer[index * 4],
                   sizeof(float) * 4);
        } else {
            memcpy(v->shader_input[slot], attr->inline_value,
                   sizeof(float) * 4);
        }
    }
    geometry_vertex_sync_convenience(v);
}

static bool geometry_inline_array_layout(PGRAPHState *pg,
                                         uint32_t offsets[NV2A_VERTEXSHADER_ATTRIBUTES],
                                         uint32_t *vertex_size)
{
    uint32_t offset = 0;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; ++i) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        offsets[i] = UINT32_MAX;
        if (attr->count == 0) {
            continue;
        }
        if (attr->size == 0) {
            return false;
        }
        offset = ROUND_UP(offset, attr->size);
        offsets[i] = offset;
        offset += attr->size * attr->count;
        offset = ROUND_UP(offset, attr->size);
    }
    *vertex_size = offset;
    return offset != 0;
}

static void geometry_read_inline_array_attr(PGRAPHState *pg,
                                            const uint8_t *vertex,
                                            const uint32_t offsets[NV2A_VERTEXSHADER_ATTRIBUTES],
                                            int slot, float out[4])
{
    VertexAttribute *attr = &pg->vertex_attributes[slot];
    if (attr->count == 0 || offsets[slot] == UINT32_MAX) {
        memcpy(out, attr->inline_value, sizeof(float) * 4);
        return;
    }
    if (!geometry_decode_attr_bytes(attr, vertex + offsets[slot], out)) {
        memcpy(out, attr->inline_value, sizeof(float) * 4);
    }
}

static void geometry_read_vertex_inline_array(
    PGRAPHState *pg, const uint8_t *vertex,
    const uint32_t offsets[NV2A_VERTEXSHADER_ATTRIBUTES], uint32_t index,
    uint16_t attr_mask, GeometryVertex *v)
{
    memset(v, 0, sizeof(*v));
    v->source_index = index;
    for (int slot = 0; slot < NV2A_VERTEXSHADER_ATTRIBUTES; ++slot) {
        if (!(attr_mask & (1u << slot))) {
            continue;
        }
        geometry_read_inline_array_attr(pg, vertex, offsets, slot,
                                        v->shader_input[slot]);
    }
    geometry_vertex_sync_convenience(v);
}

static bool geometry_collect_vertices_masked(NV2AState *d, GArray *vertices,
                                             GArray *segments,
                                             GeometrySourceKind *source,
                                             uint16_t attr_mask)
{
    PGRAPHState *pg = &d->pgraph;
    GeometryAttrReader readers[NV2A_VERTEXSHADER_ATTRIBUTES];

    if (pg->draw_arrays_length) {
        for (int slot = 0; slot < NV2A_VERTEXSHADER_ATTRIBUTES; ++slot) {
            if (attr_mask & (1u << slot)) {
                geometry_prepare_attr_reader(d, slot, &readers[slot]);
            }
        }
        *source = GEOM_SOURCE_DRAW_ARRAYS;
        for (uint32_t s = 0; s < pg->draw_arrays_length; ++s) {
            GeometrySegment seg = {
                .first_vertex = vertices->len,
                .vertex_count = (uint32_t)pg->draw_arrays_count[s],
            };
            for (uint32_t i = 0; i < seg.vertex_count; ++i) {
                GeometryVertex v;
                geometry_read_vertex_dma(readers, pg->draw_arrays_start[s] + i, attr_mask, &v);
                g_array_append_val(vertices, v);
            }
            g_array_append_val(segments, seg);
        }
        return vertices->len != 0;
    }

    if (pg->inline_elements_length) {
        for (int slot = 0; slot < NV2A_VERTEXSHADER_ATTRIBUTES; ++slot) {
            if (attr_mask & (1u << slot)) {
                geometry_prepare_attr_reader(d, slot, &readers[slot]);
            }
        }
        *source = GEOM_SOURCE_INLINE_ELEMENTS;
        GeometrySegment seg = { .first_vertex = 0,
                                .vertex_count = pg->inline_elements_length };
        for (uint32_t i = 0; i < pg->inline_elements_length; ++i) {
            GeometryVertex v;
            geometry_read_vertex_dma(readers, pg->inline_elements[i], attr_mask, &v);
            g_array_append_val(vertices, v);
        }
        g_array_append_val(segments, seg);
        return vertices->len != 0;
    }

    if (pg->inline_buffer_length) {
        *source = GEOM_SOURCE_INLINE_BUFFER;
        GeometrySegment seg = { .first_vertex = 0,
                                .vertex_count = pg->inline_buffer_length };
        for (uint32_t i = 0; i < pg->inline_buffer_length; ++i) {
            GeometryVertex v;
            geometry_read_vertex_inline_buffer(pg, i, attr_mask, &v);
            g_array_append_val(vertices, v);
        }
        g_array_append_val(segments, seg);
        return vertices->len != 0;
    }

    if (pg->inline_array_length) {
        *source = GEOM_SOURCE_INLINE_ARRAY;
        uint32_t offsets[NV2A_VERTEXSHADER_ATTRIBUTES];
        uint32_t vertex_size = 0;
        if (!geometry_inline_array_layout(pg, offsets, &vertex_size)) {
            return false;
        }
        uint64_t total_bytes = (uint64_t)pg->inline_array_length * 4;
        uint32_t count = (uint32_t)(total_bytes / vertex_size);
        if (count == 0) {
            return false;
        }
        GeometrySegment seg = { .first_vertex = 0, .vertex_count = count };
        const uint8_t *base = (const uint8_t *)pg->inline_array;
        for (uint32_t i = 0; i < count; ++i) {
            GeometryVertex v;
            geometry_read_vertex_inline_array(pg, base + (size_t)i * vertex_size,
                                              offsets, i, attr_mask, &v);
            g_array_append_val(vertices, v);
        }
        g_array_append_val(segments, seg);
        return true;
    }

    *source = GEOM_SOURCE_NONE;
    return false;
}

static bool geometry_collect_vertices(NV2AState *d, GArray *vertices,
                                      GArray *segments,
                                      GeometrySourceKind *source)
{
    return geometry_collect_vertices_masked(d, vertices, segments, source,
                                            UINT16_MAX);
}

static float geometry_const_float(PGRAPHState *pg, unsigned int row,
                                  unsigned int component)
{
    uint32_t raw = pg->vsh_constants[row][component];
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static void geometry_mul_row_vec_matrix(PGRAPHState *pg, unsigned int base,
                                        const float in[4], float out[4])
{
    /* GLSL constructs mat4(c[base], c[base+1], ...), so each c[] register is
     * one matrix column. Fixed-function NV2A uses row-vector multiplication. */
    for (unsigned int col = 0; col < 4; ++col) {
        float sum = 0.0f;
        for (unsigned int row = 0; row < 4; ++row) {
            sum += in[row] * geometry_const_float(pg, base + col, row);
        }
        out[col] = sum;
    }
}

static bool geometry_skinning_layout(PGRAPHState *pg, bool *mix,
                                     unsigned int *count)
{
    unsigned int skinning = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                                     NV_PGRAPH_CSV0_D_SKIN);
    switch (skinning) {
    case 0: *mix = false; *count = 0; return true;
    case 1: *mix = true;  *count = 2; return true;
    case 2: *mix = false; *count = 2; return true;
    case 3: *mix = true;  *count = 3; return true;
    case 4: *mix = false; *count = 3; return true;
    case 5: *mix = true;  *count = 4; return true;
    case 6: *mix = false; *count = 4; return true;
    default: return false;
    }
}

static bool geometry_fixed_function_transform(PGRAPHState *pg,
                                               const GeometryVertex *src,
                                               GeometryVertex *dst)
{
    unsigned int transform_mode = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_D), NV_PGRAPH_CSV0_D_MODE);
    if (transform_mode != 0) {
        /* Programmable Xbox vertex shaders can implement arbitrary transforms;
         * no generic world/model matrix exists to recover from all programs. */
        return false;
    }

    bool mix = false;
    unsigned int count = 0;
    if (!geometry_skinning_layout(pg, &mix, &count)) {
        return false;
    }

    *dst = *src;
    memset(dst->position, 0, sizeof(dst->position));
    memset(dst->normal, 0, sizeof(dst->normal));

    unsigned int matrix_count = count ? count : 1;
    float generated_last_weight = 1.0f;
    if (mix && matrix_count > 1) {
        for (unsigned int i = 0; i + 1 < matrix_count; ++i) {
            generated_last_weight -= src->weight[i];
        }
    }

    const float normal_in[4] = {
        src->normal[0], src->normal[1], src->normal[2], 0.0f,
    };

    for (unsigned int i = 0; i < matrix_count; ++i) {
        float weight = 1.0f;
        if (count) {
            weight = (mix && i + 1 == matrix_count)
                         ? generated_last_weight : src->weight[i];
        }

        float p[4], n[4];
        geometry_mul_row_vec_matrix(
            pg, NV_IGRAPH_XF_XFCTX_MMAT0 + i * 8, src->position, p);
        geometry_mul_row_vec_matrix(
            pg, NV_IGRAPH_XF_XFCTX_IMMAT0 + i * 8, normal_in, n);
        for (unsigned int c = 0; c < 4; ++c) {
            dst->position[c] += p[c] * weight;
            dst->normal[c] += n[c] * weight;
        }
    }

    if (pgraph_reg_r(pg, NV_PGRAPH_CSV0_C) &
        NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE) {
        float len = sqrtf(dst->normal[0] * dst->normal[0] +
                          dst->normal[1] * dst->normal[1] +
                          dst->normal[2] * dst->normal[2]);
        if (len > 0.0f && isfinite(len)) {
            dst->normal[0] /= len;
            dst->normal[1] /= len;
            dst->normal[2] /= len;
        }
    }
    return true;
}


#define GEOMETRY_VSH_CACHE_SIZE 32

typedef struct GeometryVshCacheEntry {
    bool valid;
    bool program_ready;
    unsigned int program_start;
    unsigned int program_slots;
    uint64_t source_hash;
    uint64_t last_use;
    uint16_t input_mask;
    uint64_t context_mask[3];
    bool has_relative_context;
    Nv2aVshProgram program;
    uint32_t source[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][4];
} GeometryVshCacheEntry;

typedef struct GeometryFixedFunctionContext {
    bool valid;
    bool mix;
    bool normalize_normals;
    bool need_normal;
    unsigned int skin_count;
    unsigned int matrix_count;
    float model_matrix[4][4][4];
    float inverse_model_matrix[4][4][4];
    enum VshTexgen texgen[NV2A_MAX_TEXTURES][4];
    float texgen_plane[NV2A_MAX_TEXTURES][4][4];
    bool texture_matrix_enable[NV2A_MAX_TEXTURES];
    float texture_matrix[NV2A_MAX_TEXTURES][4][4];
} GeometryFixedFunctionContext;

typedef struct GeometryVshEvalContext {
    unsigned int transform_mode;
    const GeometryVshCacheEntry *cached;
    GeometryFixedFunctionContext fixed;
} GeometryVshEvalContext;

static GeometryVshCacheEntry g_geometry_vsh_cache[GEOMETRY_VSH_CACHE_SIZE];
static uint64_t g_geometry_vsh_cache_clock;

static enum VshTexgen geometry_texgen_mode(PGRAPHState *pg, int stage,
                                           int component)
{
    unsigned int reg = stage < 2 ? NV_PGRAPH_CSV1_A : NV_PGRAPH_CSV1_B;
    unsigned int masks[4] = {
        (stage % 2) ? NV_PGRAPH_CSV1_A_T1_S : NV_PGRAPH_CSV1_A_T0_S,
        (stage % 2) ? NV_PGRAPH_CSV1_A_T1_T : NV_PGRAPH_CSV1_A_T0_T,
        (stage % 2) ? NV_PGRAPH_CSV1_A_T1_R : NV_PGRAPH_CSV1_A_T0_R,
        (stage % 2) ? NV_PGRAPH_CSV1_A_T1_Q : NV_PGRAPH_CSV1_A_T0_Q,
    };
    return (enum VshTexgen)GET_MASK(pgraph_reg_r(pg, reg), masks[component]);
}

static void geometry_copy_matrix(PGRAPHState *pg, unsigned int base,
                                 float out[4][4])
{
    for (unsigned int col = 0; col < 4; ++col) {
        for (unsigned int row = 0; row < 4; ++row) {
            out[col][row] = geometry_const_float(pg, base + col, row);
        }
    }
}

static void geometry_mul_row_vec_matrix_cached(const float matrix[4][4],
                                               const float in[4],
                                               float out[4])
{
    /* Preserve the exact multiply/add order used by
     * geometry_mul_row_vec_matrix(). */
    for (unsigned int col = 0; col < 4; ++col) {
        float sum = 0.0f;
        for (unsigned int row = 0; row < 4; ++row) {
            sum += in[row] * matrix[col][row];
        }
        out[col] = sum;
    }
}

static bool geometry_fixed_context_init(PGRAPHState *pg, uint8_t stage_mask,
                                        GeometryFixedFunctionContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    if (!geometry_skinning_layout(pg, &ctx->mix, &ctx->skin_count)) {
        return false;
    }
    ctx->matrix_count = ctx->skin_count ? ctx->skin_count : 1;
    if (ctx->matrix_count > 4) {
        return false;
    }

    for (unsigned int i = 0; i < ctx->matrix_count; ++i) {
        geometry_copy_matrix(pg, NV_IGRAPH_XF_XFCTX_MMAT0 + i * 8,
                             ctx->model_matrix[i]);
    }

    for (int stage = 0; stage < NV2A_MAX_TEXTURES; ++stage) {
        if (!(stage_mask & (1u << stage))) {
            continue;
        }
        for (int component = 0; component < 4; ++component) {
            enum VshTexgen mode = geometry_texgen_mode(pg, stage, component);
            ctx->texgen[stage][component] = mode;
            if (mode == TEXGEN_EYE_LINEAR || mode == TEXGEN_OBJECT_LINEAR) {
                for (int c = 0; c < 4; ++c) {
                    ctx->texgen_plane[stage][component][c] =
                        geometry_const_float(
                            pg, NV_IGRAPH_XF_XFCTX_TG0MAT + stage * 8 +
                                    component,
                            c);
                }
            } else if (mode == TEXGEN_SPHERE_MAP ||
                       mode == TEXGEN_REFLECTION_MAP ||
                       mode == TEXGEN_NORMAL_MAP) {
                ctx->need_normal = true;
            }
        }
        ctx->texture_matrix_enable[stage] = pg->texture_matrix_enable[stage];
        if (ctx->texture_matrix_enable[stage]) {
            geometry_copy_matrix(pg,
                                 NV_IGRAPH_XF_XFCTX_T0MAT + stage * 8,
                                 ctx->texture_matrix[stage]);
        }
    }

    ctx->normalize_normals = ctx->need_normal &&
        (pgraph_reg_r(pg, NV_PGRAPH_CSV0_C) &
         NV_PGRAPH_CSV0_C_NORMALIZATION_ENABLE);
    if (ctx->need_normal) {
        for (unsigned int i = 0; i < ctx->matrix_count; ++i) {
            geometry_copy_matrix(pg,
                                 NV_IGRAPH_XF_XFCTX_IMMAT0 + i * 8,
                                 ctx->inverse_model_matrix[i]);
        }
    }
    ctx->valid = true;
    return true;
}

static bool geometry_fixed_context_transform(
    const GeometryFixedFunctionContext *ctx, const GeometryVertex *src,
    GeometryVertex *dst)
{
    if (!ctx || !ctx->valid) {
        return false;
    }

    *dst = *src;
    memset(dst->position, 0, sizeof(dst->position));
    memset(dst->normal, 0, sizeof(dst->normal));

    float generated_last_weight = 1.0f;
    if (ctx->mix && ctx->matrix_count > 1) {
        for (unsigned int i = 0; i + 1 < ctx->matrix_count; ++i) {
            generated_last_weight -= src->weight[i];
        }
    }

    const float normal_in[4] = {
        src->normal[0], src->normal[1], src->normal[2], 0.0f,
    };

    for (unsigned int i = 0; i < ctx->matrix_count; ++i) {
        float weight = 1.0f;
        if (ctx->skin_count) {
            weight = (ctx->mix && i + 1 == ctx->matrix_count)
                         ? generated_last_weight
                         : src->weight[i];
        }

        float p[4];
        geometry_mul_row_vec_matrix_cached(ctx->model_matrix[i],
                                           src->position, p);
        for (unsigned int c = 0; c < 4; ++c) {
            dst->position[c] += p[c] * weight;
        }
        if (ctx->need_normal) {
            float n[4];
            geometry_mul_row_vec_matrix_cached(ctx->inverse_model_matrix[i],
                                               normal_in, n);
            for (unsigned int c = 0; c < 4; ++c) {
                dst->normal[c] += n[c] * weight;
            }
        }
    }

    if (ctx->normalize_normals) {
        float len = sqrtf(dst->normal[0] * dst->normal[0] +
                          dst->normal[1] * dst->normal[1] +
                          dst->normal[2] * dst->normal[2]);
        if (len > 0.0f && isfinite(len)) {
            dst->normal[0] /= len;
            dst->normal[1] /= len;
            dst->normal[2] /= len;
        }
    }
    return true;
}

static float geometry_dot4(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static bool geometry_normalize3(const float in[3], float out[3])
{
    float len2 = in[0] * in[0] + in[1] * in[1] + in[2] * in[2];
    if (!(len2 > 0.0f) || !isfinite(len2)) {
        out[0] = out[1] = out[2] = 0.0f;
        return false;
    }
    float inv = 1.0f / sqrtf(len2);
    out[0] = in[0] * inv;
    out[1] = in[1] * inv;
    out[2] = in[2] * inv;
    return true;
}

static void geometry_reflect3(const float incident[3], const float normal[3],
                              float out[3])
{
    float d = incident[0] * normal[0] + incident[1] * normal[1] +
              incident[2] * normal[2];
    out[0] = incident[0] - 2.0f * d * normal[0];
    out[1] = incident[1] - 2.0f * d * normal[1];
    out[2] = incident[2] - 2.0f * d * normal[2];
}

static bool geometry_eval_fixed_texcoords(
    const GeometryFixedFunctionContext *ctx, GeometryVertex *vertex,
    uint8_t stage_mask)
{
    GeometryVertex transformed;
    if (!geometry_fixed_context_transform(ctx, vertex, &transformed)) {
        return false;
    }

    vertex->material_position[0] = transformed.position[0];
    vertex->material_position[1] = transformed.position[1];
    vertex->material_position[2] = transformed.position[2];
    vertex->material_position_valid =
        isfinite(transformed.position[0]) && isfinite(transformed.position[1]) &&
        isfinite(transformed.position[2]);

    for (int stage = 0; stage < NV2A_MAX_TEXTURES; ++stage) {
        if (!(stage_mask & (1u << stage))) {
            continue;
        }
        float out[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        bool stage_valid = true;
        float incident[3] = { 0.0f, 0.0f, 0.0f };
        float reflected[3] = { 0.0f, 0.0f, 0.0f };
        bool reflection_ready = false;

        for (int component = 0; component < 4; ++component) {
            enum VshTexgen mode = ctx->texgen[stage][component];
            switch (mode) {
            case TEXGEN_DISABLE:
                out[component] =
                    vertex->shader_input[NV2A_VERTEX_ATTR_TEXTURE0 + stage]
                                        [component];
                break;
            case TEXGEN_EYE_LINEAR:
                out[component] = geometry_dot4(
                    ctx->texgen_plane[stage][component], transformed.position);
                break;
            case TEXGEN_OBJECT_LINEAR:
                out[component] = geometry_dot4(
                    ctx->texgen_plane[stage][component], vertex->position);
                break;
            case TEXGEN_SPHERE_MAP:
            case TEXGEN_REFLECTION_MAP:
                if (!reflection_ready) {
                    float tp[3] = { transformed.position[0],
                                    transformed.position[1],
                                    transformed.position[2] };
                    if (!geometry_normalize3(tp, incident)) {
                        stage_valid = false;
                        break;
                    }
                    geometry_reflect3(incident, transformed.normal, reflected);
                    reflection_ready = true;
                }
                if (!stage_valid) {
                    break;
                }
                if (mode == TEXGEN_REFLECTION_MAP) {
                    if (component >= 3) {
                        stage_valid = false;
                    } else {
                        out[component] = reflected[component];
                    }
                } else {
                    if (component >= 2) {
                        stage_valid = false;
                    } else {
                        float rx = reflected[0];
                        float ry = reflected[1];
                        float rz = reflected[2] + 1.0f;
                        float len = sqrtf(rx * rx + ry * ry + rz * rz);
                        if (!(len > 0.0f) || !isfinite(len)) {
                            stage_valid = false;
                        } else {
                            float inv_m = 1.0f / (2.0f * len);
                            out[component] = reflected[component] * inv_m + 0.5f;
                        }
                    }
                }
                break;
            case TEXGEN_NORMAL_MAP:
                if (component >= 3) {
                    stage_valid = false;
                } else {
                    out[component] = transformed.normal[component];
                }
                break;
            default:
                stage_valid = false;
                break;
            }
            if (!stage_valid) {
                break;
            }
        }

        if (stage_valid && ctx->texture_matrix_enable[stage]) {
            float transformed_tc[4];
            geometry_mul_row_vec_matrix_cached(ctx->texture_matrix[stage],
                                               out, transformed_tc);
            memcpy(out, transformed_tc, sizeof(out));
        }

        for (int c = 0; c < 4; ++c) {
            if (!isfinite(out[c])) {
                stage_valid = false;
                break;
            }
        }
        if (stage_valid) {
            memcpy(vertex->post_vsh_texcoord[stage], out, sizeof(out));
            vertex->post_vsh_texcoord_valid[stage] = true;
        }
    }
    return true;
}

static void geometry_vsh_mask_context(uint64_t mask[3], unsigned int index)
{
    if (index < NV2A_VERTEXSHADER_CONSTANTS) {
        mask[index >> 6] |= 1ULL << (index & 63);
    }
}

static void geometry_vsh_analyze_step(const Nv2aVshStep *step,
                                      GeometryVshCacheEntry *entry)
{
    const Nv2aVshOperation *ops[2] = { &step->mac, &step->ilu };
    for (int which = 0; which < 2; ++which) {
        const Nv2aVshOperation *op = ops[which];
        for (int i = 0; i < 3; ++i) {
            const Nv2aVshInput *in = &op->inputs[i];
            if (in->type == NV2ART_INPUT &&
                in->index < NV2A_VERTEXSHADER_ATTRIBUTES) {
                entry->input_mask |= (uint16_t)(1u << in->index);
            } else if (in->type == NV2ART_CONTEXT) {
                if (in->is_relative) {
                    entry->has_relative_context = true;
                } else {
                    geometry_vsh_mask_context(entry->context_mask, in->index);
                }
            }
        }
        /* A partial write to c[] must preserve untouched components, so copy
         * the original destination register too. */
        for (int i = 0; i < 2; ++i) {
            const Nv2aVshOutput *out = &op->outputs[i];
            if (out->type == NV2ART_CONTEXT) {
                geometry_vsh_mask_context(entry->context_mask, out->index);
            }
        }
    }
}

static GeometryVshCacheEntry *geometry_vsh_cache_get(PGRAPHState *pg,
                                                      unsigned int start)
{
    size_t source_slots = NV2A_MAX_TRANSFORM_PROGRAM_LENGTH - start;
    size_t source_bytes = source_slots * sizeof(pg->program_data[0]);
    const uint8_t *source = (const uint8_t *)&pg->program_data[start][0];
    uint64_t hash = fast_hash(source, source_bytes);
    uint64_t stamp = ++g_geometry_vsh_cache_clock;

    for (int i = 0; i < GEOMETRY_VSH_CACHE_SIZE; ++i) {
        GeometryVshCacheEntry *entry = &g_geometry_vsh_cache[i];
        if (!entry->valid || entry->program_start != start ||
            entry->source_hash != hash) {
            continue;
        }
        if (memcmp(entry->source, source, source_bytes) == 0) {
            entry->last_use = stamp;
            return entry;
        }
    }

    GeometryVshCacheEntry *entry = NULL;
    for (int i = 0; i < GEOMETRY_VSH_CACHE_SIZE; ++i) {
        GeometryVshCacheEntry *candidate = &g_geometry_vsh_cache[i];
        if (!candidate->valid) {
            entry = candidate;
            break;
        }
        if (!entry || candidate->last_use < entry->last_use) {
            entry = candidate;
        }
    }
    assert(entry != NULL);
    if (entry->valid && entry->program_ready) {
        nv2a_vsh_program_destroy(&entry->program);
    }
    memset(entry, 0, sizeof(*entry));
    entry->valid = true;
    entry->program_start = start;
    entry->source_hash = hash;
    entry->last_use = stamp;
    memcpy(entry->source, source, source_bytes);

    unsigned int slots = 0;
    for (unsigned int slot = 0; slot < source_slots; ++slot) {
        Nv2aVshStep step;
        memset(&step, 0, sizeof(step));
        if (nv2a_vsh_parse_step(&step, pg->program_data[start + slot]) !=
            NV2AVPR_SUCCESS) {
            return entry; /* Cache the rejection as well. */
        }
        geometry_vsh_analyze_step(&step, entry);
        slots = slot + 1;
        if (step.is_final) {
            break;
        }
    }
    if (slots == 0) {
        return entry;
    }
    Nv2aVshStep final_step;
    memset(&final_step, 0, sizeof(final_step));
    if (nv2a_vsh_parse_step(&final_step,
                            pg->program_data[start + slots - 1]) !=
            NV2AVPR_SUCCESS ||
        !final_step.is_final) {
        return entry;
    }

    entry->program_slots = slots;
    entry->program_ready = nv2a_vsh_parse_program(
        &entry->program, pg->program_data[start], slots) == NV2AVPR_SUCCESS;
    return entry;
}

static bool geometry_vsh_eval_context_init(PGRAPHState *pg, uint8_t stage_mask,
                                           GeometryVshEvalContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->transform_mode = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                                   NV_PGRAPH_CSV0_D_MODE);
    if (ctx->transform_mode == 0) {
        return geometry_fixed_context_init(pg, stage_mask, &ctx->fixed);
    }
    if (ctx->transform_mode != 2) {
        return false;
    }

    unsigned int program_start = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
        NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
    if (program_start >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        return false;
    }
    ctx->cached = geometry_vsh_cache_get(pg, program_start);
    return ctx->cached && ctx->cached->program_ready;
}

static void geometry_vsh_eval_context_destroy(GeometryVshEvalContext *ctx)
{
    /* Programs are owned by the renderer-thread LRU and intentionally retained
     * across draws. */
    (void)ctx;
}

static bool geometry_eval_programmable_texcoords(
    PGRAPHState *pg, const GeometryVshEvalContext *ctx, GeometryVertex *vertex,
    uint8_t stage_mask)
{
    if (!ctx || !ctx->cached || !ctx->cached->program_ready) {
        return false;
    }

    const GeometryVshCacheEntry *cached = ctx->cached;
    Nv2aVshCPUFullExecutionState storage;
    Nv2aVshExecutionState state =
        nv2a_vsh_emu_initialize_full_execution_state(&storage);

    uint16_t inputs = cached->input_mask;
    while (inputs) {
        unsigned int slot = __builtin_ctz((unsigned int)inputs);
        memcpy(&state.input_regs[slot * 4], vertex->shader_input[slot],
               sizeof(float) * 4);
        inputs &= (uint16_t)(inputs - 1);
    }

    /* pg->vsh_constants are IEEE-754 bit patterns; copy only registers the
     * shader can directly read/partially overwrite. Relative c[A0+n] access
     * conservatively falls back to the complete bank. */
    if (cached->has_relative_context) {
        memcpy(state.context_regs, pg->vsh_constants, sizeof(pg->vsh_constants));
    } else {
        for (unsigned int word = 0; word < 3; ++word) {
            uint64_t mask = cached->context_mask[word];
            while (mask) {
                unsigned int bit = (unsigned int)__builtin_ctzll(mask);
                unsigned int reg = word * 64 + bit;
                memcpy(&state.context_regs[reg * 4], pg->vsh_constants[reg],
                       sizeof(float) * 4);
                mask &= mask - 1;
            }
        }
    }
    nv2a_vsh_emu_execute(&state, &cached->program);

    const float *position = &state.output_regs[NV2AOR_POS * 4];
    bool position_valid = true;
    for (int c = 0; c < 4; ++c) {
        if (!isfinite(position[c])) {
            position_valid = false;
            break;
        }
    }
    if (position_valid) {
        memcpy(vertex->post_vsh_position, position,
               sizeof(vertex->post_vsh_position));
        vertex->post_vsh_position_valid = true;
        vertex->material_position[0] = position[0];
        vertex->material_position[1] = position[1];
        vertex->material_position[2] = position[3];
        vertex->material_position_valid = isfinite(position[0]) &&
                                          isfinite(position[1]) &&
                                          isfinite(position[3]);
    }

    for (int stage = 0; stage < NV2A_MAX_TEXTURES; ++stage) {
        if (!(stage_mask & (1u << stage))) {
            continue;
        }
        const float *src = &state.output_regs[(NV2AOR_TEX0 + stage) * 4];
        bool valid = true;
        for (int c = 0; c < 4; ++c) {
            if (!isfinite(src[c])) {
                valid = false;
                break;
            }
        }
        if (valid) {
            memcpy(vertex->post_vsh_texcoord[stage], src,
                   sizeof(vertex->post_vsh_texcoord[stage]));
            vertex->post_vsh_texcoord_valid[stage] = true;
        }
    }
    return true;
}

static bool geometry_evaluate_post_vsh_texcoords_masked(
    PGRAPHState *pg, GArray *vertices, uint8_t stage_mask,
    const GeometryVshEvalContext *prepared_ctx)
{
    if (!pg || !vertices || vertices->len == 0 || stage_mask == 0) {
        return false;
    }
    GeometryVshEvalContext local_ctx;
    const GeometryVshEvalContext *ctx = prepared_ctx;
    if (!ctx) {
        if (!geometry_vsh_eval_context_init(pg, stage_mask, &local_ctx)) {
            return false;
        }
        ctx = &local_ctx;
    }

    bool any = false;
    for (guint i = 0; i < vertices->len; ++i) {
        GeometryVertex *v = &g_array_index(vertices, GeometryVertex, i);
        bool ok = ctx->transform_mode == 0
                      ? geometry_eval_fixed_texcoords(&ctx->fixed, v, stage_mask)
                      : geometry_eval_programmable_texcoords(pg, ctx, v,
                                                             stage_mask);
        any |= ok;
    }
    if (!prepared_ctx) {
        geometry_vsh_eval_context_destroy(&local_ctx);
    }
    return any;
}

static bool geometry_evaluate_post_vsh_texcoords(PGRAPHState *pg,
                                                  GArray *vertices)
{
    return geometry_evaluate_post_vsh_texcoords_masked(
        pg, vertices, (1u << NV2A_MAX_TEXTURES) - 1u, NULL);
}


static float geometry_material_dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool geometry_material_normalize3(float v[3])
{
    float len2 = geometry_material_dot3(v, v);
    if (!(len2 > 1.0e-12f) || !isfinite(len2)) {
        return false;
    }
    float inv_len = 1.0f / sqrtf(len2);
    v[0] *= inv_len;
    v[1] *= inv_len;
    v[2] *= inv_len;
    return true;
}

static void geometry_material_cross3(const float a[3], const float b[3],
                                     float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static bool geometry_material_uv(const GeometryVertex *v, int stage,
                                 float out[2])
{
    if (!v || stage < 0 || stage >= NV2A_MAX_TEXTURES ||
        !v->post_vsh_texcoord_valid[stage]) {
        return false;
    }

    const float *t = v->post_vsh_texcoord[stage];
    float q = t[3];
    if (isfinite(q) && fabsf(q) > 1.0e-8f) {
        out[0] = t[0] / q;
        out[1] = t[1] / q;
    } else {
        out[0] = t[0];
        out[1] = t[1];
    }
    return isfinite(out[0]) && isfinite(out[1]);
}

static bool geometry_material_position(PGRAPHState *pg,
                                       const GeometryVertex *v,
                                       bool fixed_function,
                                       float out[3])
{
    /* Evaluation already computes the appropriate stable position once per
     * vertex. Reusing it here avoids repeating fixed-function skinning/matrix
     * work for every triangle and every enhanced texture stage. */
    (void)pg;
    (void)fixed_function;
    if (!v || !v->material_position_valid) {
        return false;
    }
    memcpy(out, v->material_position, sizeof(v->material_position));
    return true;
}


static bool geometry_material_light_from_triangle(
    PGRAPHState *pg, const GeometryVertex *a, const GeometryVertex *b,
    const GeometryVertex *c, int stage, bool fixed_function, float out[3],
    float *out_weight)
{
    float p0[3], p1[3], p2[3];
    float uv0[2], uv1[2], uv2[2];
    if (!geometry_material_position(pg, a, fixed_function, p0) ||
        !geometry_material_position(pg, b, fixed_function, p1) ||
        !geometry_material_position(pg, c, fixed_function, p2) ||
        !geometry_material_uv(a, stage, uv0) ||
        !geometry_material_uv(b, stage, uv1) ||
        !geometry_material_uv(c, stage, uv2)) {
        return false;
    }

    float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
    float area_cross[3];
    geometry_material_cross3(e1, e2, area_cross);
    float area2 = sqrtf(geometry_material_dot3(area_cross, area_cross));
    if (!isfinite(area2) || area2 <= 1.0e-8f) {
        return false;
    }
    float du1 = uv1[0] - uv0[0];
    float dv1 = uv1[1] - uv0[1];
    float du2 = uv2[0] - uv0[0];
    float dv2 = uv2[1] - uv0[1];
    float det = du1 * dv2 - dv1 * du2;
    if (!isfinite(det) || fabsf(det) <= 1.0e-10f) {
        return false;
    }

    float inv_det = 1.0f / det;
    float tangent[3] = {
        (e1[0] * dv2 - e2[0] * dv1) * inv_det,
        (e1[1] * dv2 - e2[1] * dv1) * inv_det,
        (e1[2] * dv2 - e2[2] * dv1) * inv_det,
    };
    float bitangent_raw[3] = {
        (-e1[0] * du2 + e2[0] * du1) * inv_det,
        (-e1[1] * du2 + e2[1] * du1) * inv_det,
        (-e1[2] * du2 + e2[2] * du1) * inv_det,
    };

    /* Build a camera-independent orthonormal TBN. V6 made the basis face the
     * camera by conditionally negating B/N. That necessarily changes sign
     * when a surface passes through grazing incidence and is the source of
     * the remaining far-tilt normal-map flip. Geometry winding and UV
     * handedness must define the tangent frame; the camera must never do so. */
    float normal[3] = { area_cross[0], area_cross[1], area_cross[2] };
    if (!geometry_material_normalize3(normal)) {
        return false;
    }

    float tangent_dot_n = geometry_material_dot3(tangent, normal);
    tangent[0] -= normal[0] * tangent_dot_n;
    tangent[1] -= normal[1] * tangent_dot_n;
    tangent[2] -= normal[2] * tangent_dot_n;
    if (!geometry_material_normalize3(tangent)) {
        return false;
    }

    float bitangent[3];
    geometry_material_cross3(normal, tangent, bitangent);
    if (!geometry_material_normalize3(bitangent)) {
        return false;
    }
    if (geometry_material_dot3(bitangent, bitangent_raw) < 0.0f) {
        bitangent[0] = -bitangent[0];
        bitangent[1] = -bitangent[1];
        bitangent[2] = -bitangent[2];
    }

    /* Camera-reactive view/headlight direction. A camera-mounted directional
     * light and the view direction share the fixed +Z view-space axis.
     * Project that axis into the stable TBN. For two-sided guest geometry,
     * keep the normal-map hemisphere positive with abs(N dot V) instead of
     * flipping the tangent frame. This is continuous through edge-on views:
     * X/Y retain their orientation and Z simply approaches zero and returns. */
    float camera_axis[3] = { 0.0f, 0.0f, 1.0f };
    out[0] = geometry_material_dot3(camera_axis, tangent);
    out[1] = geometry_material_dot3(camera_axis, bitangent);
    out[2] = fabsf(geometry_material_dot3(camera_axis, normal));
    if (!geometry_material_normalize3(out)) {
        return false;
    }
    if (out_weight != NULL) {
        /* Geometric area is a stable proxy for how representative a triangle
         * is. Tiny seam/degenerate triangles no longer get to drive the whole
         * texture's camera-reactive light direction. */
        *out_weight = area2;
    }
    return true;
}

static bool geometry_material_try_segment(PGRAPHState *pg,
                                          const GArray *vertices,
                                          const GeometrySegment *seg,
                                          uint32_t primitive, int stage,
                                          bool fixed_function, float out[3],
                                          float *out_weight)
{
    if (!vertices || !seg || seg->vertex_count < 3) {
        return false;
    }

    uint32_t first = seg->first_vertex;
    uint32_t count = seg->vertex_count;
    float accum[3] = { 0.0f, 0.0f, 0.0f };
    float total_weight = 0.0f;
    float best_dir[3] = { 0.0f, 0.0f, 1.0f };
    float best_weight = 0.0f;
    unsigned int samples = 0;
    const unsigned int max_samples = 256;

#define ACCUM_TRI(A, B, C) do { \
        if (samples >= max_samples) { break; } \
        const GeometryVertex *va = &g_array_index(vertices, GeometryVertex, (A)); \
        const GeometryVertex *vb = &g_array_index(vertices, GeometryVertex, (B)); \
        const GeometryVertex *vc = &g_array_index(vertices, GeometryVertex, (C)); \
        float tri_dir[3]; \
        float tri_weight = 0.0f; \
        if (geometry_material_light_from_triangle(pg, va, vb, vc, stage, \
                                                  fixed_function, tri_dir, \
                                                  &tri_weight)) { \
            accum[0] += tri_dir[0] * tri_weight; \
            accum[1] += tri_dir[1] * tri_weight; \
            accum[2] += tri_dir[2] * tri_weight; \
            total_weight += tri_weight; \
            if (tri_weight > best_weight) { \
                best_weight = tri_weight; \
                memcpy(best_dir, tri_dir, sizeof(best_dir)); \
            } \
            samples++; \
        } \
    } while (0)

    switch (primitive) {
    case PRIM_TYPE_TRIANGLES:
        for (uint32_t i = 0; i + 2 < count && samples < max_samples; i += 3) {
            ACCUM_TRI(first + i, first + i + 1, first + i + 2);
        }
        break;
    case PRIM_TYPE_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < count && samples < max_samples; ++i) {
            /* Alternate strip winding so the representative orientation does
             * not flip every triangle. */
            if (i & 1) {
                ACCUM_TRI(first + i + 1, first + i, first + i + 2);
            } else {
                ACCUM_TRI(first + i, first + i + 1, first + i + 2);
            }
        }
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
        for (uint32_t i = 1; i + 1 < count && samples < max_samples; ++i) {
            ACCUM_TRI(first, first + i, first + i + 1);
        }
        break;
    case PRIM_TYPE_QUADS:
        for (uint32_t i = 0; i + 3 < count && samples < max_samples; i += 4) {
            ACCUM_TRI(first + i, first + i + 1, first + i + 2);
            ACCUM_TRI(first + i, first + i + 2, first + i + 3);
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < count && samples < max_samples; i += 2) {
            ACCUM_TRI(first + i, first + i + 1, first + i + 2);
            ACCUM_TRI(first + i + 1, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;
    }
#undef ACCUM_TRI

    if (total_weight <= 0.0f) {
        return false;
    }

    out[0] = accum[0];
    out[1] = accum[1];
    out[2] = accum[2];
    if (!geometry_material_normalize3(out)) {
        memcpy(out, best_dir, sizeof(best_dir));
    }
    if (out_weight != NULL) {
        *out_weight = total_weight;
    }
    return true;
}

typedef struct GeometryMaterialHashCache {
    bool valid;
    hwaddr texture_offset;
    size_t texture_length;
    hwaddr palette_offset;
    size_t palette_length;
    unsigned int color_format;
    unsigned int width;
    unsigned int height;
    int last_hash_frame_time;
    uint64_t hash;
} GeometryMaterialHashCache;

static GeometryMaterialHashCache
    g_geometry_material_hash_cache[NV2A_MAX_TEXTURES];
/* Renderer-thread scratch retained across draws. The material observer is not
 * re-entrant, so this removes two GArray allocate/free pairs per enhanced draw
 * without changing the full-fidelity explicit Geometry Dumper path. */
static GArray *g_geometry_material_vertices;
static GArray *g_geometry_material_segments;

static bool geometry_material_texture_hash(NV2AState *d, int stage,
                                           uint64_t *out_hash)
{
    if (d == NULL || out_hash == NULL || stage < 0 ||
        stage >= NV2A_MAX_TEXTURES) {
        return false;
    }

    PGRAPHState *pg = &d->pgraph;
    if (!pgraph_is_texture_enabled(pg, stage)) {
        return false;
    }

    TextureShape shape = pgraph_get_texture_shape(pg, stage);
    if (shape.cubemap || shape.dimensionality != 2 || shape.width == 0 ||
        shape.height == 0 || shape.color_format >= 66 ||
        kelvin_color_format_info_map[shape.color_format].depth) {
        return false;
    }

    size_t texture_length = pgraph_get_texture_length(pg, &shape);
    hwaddr texture_offset = pgraph_get_texture_phys_addr(pg, stage);
    size_t vram_size = memory_region_size(d->vram);
    if (texture_offset >= vram_size ||
        texture_length > vram_size - texture_offset) {
        return false;
    }

    size_t palette_length = 0;
    hwaddr palette_offset = 0;
    if (shape.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
        palette_offset = pgraph_get_texture_palette_phys_addr_length(
            pg, stage, &palette_length);
        if (palette_offset >= vram_size ||
            palette_length > vram_size - palette_offset) {
            return false;
        }
    }

    GeometryMaterialHashCache *cache = &g_geometry_material_hash_cache[stage];
    bool same_resource = cache->valid &&
        cache->texture_offset == texture_offset &&
        cache->texture_length == texture_length &&
        cache->palette_offset == palette_offset &&
        cache->palette_length == palette_length &&
        cache->color_format == shape.color_format &&
        cache->width == shape.width && cache->height == shape.height;

    if (same_resource) {
        /* Texture methods mark a stage dirty even for sampler/control changes
         * and even when a game simply reissues the same state. Hashing the
         * complete guest texture on every such draw is unnecessarily costly.
         *
         * Revalidate a dirty resource at most once per guest frame. A hash
         * that currently owns material sidecars is also revalidated once per
         * frame so in-place guest VRAM updates cannot leave camera tracking
         * attached to stale content forever. Non-material resources receive a
         * low-frequency safety recheck to catch the same case without turning
         * every ordinary texture into a per-frame hashing cost. */
        uint32_t hash_age = (uint32_t)pg->frame_time -
                            (uint32_t)cache->last_hash_frame_time;
        bool cached_material = cache->hash != 0 &&
            xemu_texture_packs_material_sidecars_present(cache->hash);
        bool periodic_recheck = cached_material ? hash_age >= 1u
                                                : hash_age >= 60u;
        if (cache->last_hash_frame_time == pg->frame_time ||
            (!pg->texture_dirty[stage] && !periodic_recheck)) {
            *out_hash = cache->hash;
            return cache->hash != 0;
        }
    }

    uint64_t hash = fast_hash(d->vram_ptr + texture_offset, texture_length);
    if (palette_length != 0) {
        hash ^= fast_hash(d->vram_ptr + palette_offset, palette_length);
    }

    cache->valid = true;
    cache->texture_offset = texture_offset;
    cache->texture_length = texture_length;
    cache->palette_offset = palette_offset;
    cache->palette_length = palette_length;
    cache->color_format = shape.color_format;
    cache->width = shape.width;
    cache->height = shape.height;
    cache->last_hash_frame_time = pg->frame_time;
    cache->hash = hash;

    *out_hash = hash;
    return hash != 0;
}

static void geometry_material_update_camera_headlight(NV2AState *d)
{
    if (!d || !xemu_texture_packs_material_camera_tracking_needed()) {
        return;
    }

    PGRAPHState *pg = &d->pgraph;
    if (pg->primitive_mode == PRIM_TYPE_INVALID) {
        return;
    }

    /* Resolve the actual texture hashes first. This avoids doing expensive
     * vertex collection/TBN work for ordinary draws that do not use a
     * material-map sidecar. It also makes the resulting light state belong to
     * the texture itself instead of transient NV2A stage numbers. */
    uint64_t stage_hash[NV2A_MAX_TEXTURES] = { 0 };
    uint8_t stage_mask = 0;
    bool any_material_stage = false;
    for (int stage = 0; stage < NV2A_MAX_TEXTURES; ++stage) {
        uint64_t hash = 0;
        /* The active backend already computed the exact texture-pack identity
         * for each bound stage. Reuse it instead of re-hashing guest VRAM. A
         * hash fallback remains only for unusual timing/backend states where
         * no binding identity is available yet. */
        bool binding_known = xemu_texture_packs_material_bound_hash(
            pg, stage, &hash);
        if (!binding_known &&
            !geometry_material_texture_hash(d, stage, &hash)) {
            continue;
        }
        if (hash != 0 &&
            xemu_texture_packs_material_sidecars_present(hash)) {
            stage_hash[stage] = hash;
            stage_mask |= (uint8_t)(1u << stage);
            any_material_stage = true;
        }
    }
    if (!any_material_stage) {
        return;
    }

    if (g_geometry_material_vertices == NULL) {
        g_geometry_material_vertices =
            g_array_sized_new(FALSE, FALSE, sizeof(GeometryVertex), 256);
    }
    if (g_geometry_material_segments == NULL) {
        g_geometry_material_segments =
            g_array_sized_new(FALSE, FALSE, sizeof(GeometrySegment), 8);
    }
    GArray *vertices = g_geometry_material_vertices;
    GArray *segments = g_geometry_material_segments;
    g_array_set_size(vertices, 0);
    g_array_set_size(segments, 0);

    GeometryVshEvalContext eval_ctx;
    if (!geometry_vsh_eval_context_init(pg, stage_mask, &eval_ctx)) {
        return;
    }

    uint16_t attr_mask = 0;
    bool fixed_function = eval_ctx.transform_mode == 0;
    if (fixed_function) {
        attr_mask |= (uint16_t)(1u << NV2A_VERTEX_ATTR_POSITION);
        if (eval_ctx.fixed.skin_count != 0) {
            attr_mask |= (uint16_t)(1u << NV2A_VERTEX_ATTR_WEIGHT);
        }
        for (int stage = 0; stage < NV2A_MAX_TEXTURES; ++stage) {
            if (!(stage_mask & (1u << stage))) {
                continue;
            }
            for (int component = 0; component < 4; ++component) {
                enum VshTexgen mode =
                    eval_ctx.fixed.texgen[stage][component];
                if (mode == TEXGEN_DISABLE) {
                    attr_mask |= (uint16_t)(
                        1u << (NV2A_VERTEX_ATTR_TEXTURE0 + stage));
                } else if (mode == TEXGEN_SPHERE_MAP ||
                           mode == TEXGEN_REFLECTION_MAP ||
                           mode == TEXGEN_NORMAL_MAP) {
                    attr_mask |=
                        (uint16_t)(1u << NV2A_VERTEX_ATTR_NORMAL);
                }
            }
        }
    } else if (eval_ctx.transform_mode == 2 && eval_ctx.cached) {
        attr_mask = eval_ctx.cached->input_mask;
    } else {
        return;
    }

    GeometrySourceKind source = GEOM_SOURCE_NONE;
    if (!geometry_collect_vertices_masked(d, vertices, segments, &source,
                                          attr_mask) ||
        !geometry_evaluate_post_vsh_texcoords_masked(
            pg, vertices, stage_mask, &eval_ctx)) {
        return;
    }
    (void)source;

    for (int stage = 0; stage < NV2A_MAX_TEXTURES; ++stage) {
        uint64_t hash = stage_hash[stage];
        if (hash == 0) {
            continue;
        }

        float accum[3] = { 0.0f, 0.0f, 0.0f };
        float total_weight = 0.0f;
        float best_dir[3] = { 0.0f, 0.0f, 1.0f };
        float best_weight = 0.0f;

        for (guint i = 0; i < segments->len; ++i) {
            const GeometrySegment *seg =
                &g_array_index(segments, GeometrySegment, i);
            float seg_dir[3];
            float seg_weight = 0.0f;
            if (!geometry_material_try_segment(
                    pg, vertices, seg, pg->primitive_mode, stage,
                    fixed_function, seg_dir, &seg_weight)) {
                continue;
            }
            accum[0] += seg_dir[0] * seg_weight;
            accum[1] += seg_dir[1] * seg_weight;
            accum[2] += seg_dir[2] * seg_weight;
            total_weight += seg_weight;
            if (seg_weight > best_weight) {
                best_weight = seg_weight;
                memcpy(best_dir, seg_dir, sizeof(best_dir));
            }
        }

        if (total_weight <= 0.0f) {
            continue;
        }

        float light_ts[3] = { accum[0], accum[1], accum[2] };
        if (!geometry_material_normalize3(light_ts)) {
            memcpy(light_ts, best_dir, sizeof(light_ts));
        }
        /* Publish the area-averaged direction for this draw immediately. A
         * shared replacement hash may be used on several differently oriented
         * surfaces in the same frame; frame-wide averaging cannot represent
         * those surfaces correctly and becomes unstable as their screen/area
         * weights change. Draw-synchronous relighting lets sequential draws
         * reuse the same enhanced texture safely. */
        xemu_texture_packs_material_set_hash_view_light(hash, light_ts);

        /* Backends that bind textures before complete draw geometry is known
         * (OpenGL) can register a feature-owned immediate refresh callback.
         * Vulkan consumes the new revision later in its normal pre-draw bind. */
        xemu_texture_packs_material_refresh_draw(pg, stage, hash);
    }
}

static bool geometry_build_placed_vertices(PGRAPHState *pg,
                                           const GArray *raw,
                                           GArray *placed)
{
    if (!raw || !placed || raw->len == 0) {
        return false;
    }
    for (guint i = 0; i < raw->len; ++i) {
        const GeometryVertex *src = &g_array_index(raw, GeometryVertex, i);
        GeometryVertex dst;
        if (!geometry_fixed_function_transform(pg, src, &dst)) {
            g_array_set_size(placed, 0);
            return false;
        }
        g_array_append_val(placed, dst);
    }
    return placed->len == raw->len;
}

static void geometry_json_write_const_matrix(FILE *f, PGRAPHState *pg,
                                             unsigned int base)
{
    fputc('[', f);
    for (unsigned int col = 0; col < 4; ++col) {
        if (col) {
            fputc(',', f);
        }
        fputc('[', f);
        for (unsigned int row = 0; row < 4; ++row) {
            if (row) {
                fputc(',', f);
            }
            fprintf(f, "%.9g", geometry_const_float(pg, base + col, row));
        }
        fputc(']', f);
    }
    fputc(']', f);
}

typedef struct GeometryTextureExportInfo {
    bool enabled;
    bool stage_active;
    bool dumped;
    bool cubemap;
    bool linear;
    bool border;
    uint64_t content_hash;
    unsigned int width;
    unsigned int height;
    unsigned int decoded_width;
    unsigned int decoded_height;
    unsigned int color_format;
    unsigned int pixel_texture_mode;
    uint32_t address_reg;
    uint32_t filter_reg;
    uint32_t border_color;
    unsigned int address_u;
    unsigned int address_v;
    unsigned int min_filter;
    unsigned int mag_filter;
    int gltf_wrap_s;
    int gltf_wrap_t;
    int gltf_min_filter;
    int gltf_mag_filter;
    bool sampler_border_approx;
    bool blend_enabled;
    bool alpha_test_enabled;
    unsigned int alpha_ref;
    unsigned int alpha_func;
    char file[256];
} GeometryTextureExportInfo;

static uint8_t geometry_expand_4(uint8_t v)
{
    return (uint8_t)((v << 4) | v);
}

static uint8_t geometry_expand_5(uint8_t v)
{
    return (uint8_t)((v << 3) | (v >> 2));
}

static uint8_t geometry_expand_6(uint8_t v)
{
    return (uint8_t)((v << 2) | (v >> 4));
}

static bool geometry_convert_packed_rgba(unsigned int color_format,
                                         const uint8_t *src,
                                         unsigned int width,
                                         unsigned int height,
                                         unsigned int row_stride,
                                         uint8_t *rgba)
{
    if (!src || !rgba || width == 0 || height == 0) {
        return false;
    }

    for (unsigned int y = 0; y < height; ++y) {
        const uint8_t *row = src + (size_t)y * row_stride;
        for (unsigned int x = 0; x < width; ++x) {
            uint8_t *dst = rgba + ((size_t)y * width + x) * 4;
            switch (color_format) {
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8:
                dst[0] = dst[1] = dst[2] = row[x];
                dst[3] = 255;
                break;
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8: {
                /* Match the renderer's R8 + RGBA swizzle exactly. AY8 is not
                 * two four-bit channels on NV2A's texture sampling path. */
                uint8_t p = row[x];
                dst[0] = dst[1] = dst[2] = dst[3] = p;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8:
                dst[0] = dst[1] = dst[2] = 255;
                dst[3] = row[x];
                break;
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8: {
                uint16_t p = lduw_le_p(row + x * 2);
                uint8_t l = p & 0xff;
                dst[0] = dst[1] = dst[2] = l;
                dst[3] = p >> 8;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8: {
                const uint8_t *p = row + x * 2;
                /* GL/Vulkan use R8G8 storage with swizzle R,G,R,G. */
                dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[0]; dst[3] = p[1];
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8: {
                const uint8_t *p = row + x * 2;
                /* R8B8 uses the renderer's G,R,R,G swizzle. */
                dst[0] = p[1]; dst[1] = p[0]; dst[2] = p[0]; dst[3] = p[1];
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5:
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5: {
                uint16_t p = lduw_le_p(row + x * 2);
                dst[0] = geometry_expand_5((p >> 10) & 0x1f);
                dst[1] = geometry_expand_5((p >> 5) & 0x1f);
                dst[2] = geometry_expand_5(p & 0x1f);
                bool has_alpha =
                    color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5 ||
                    color_format == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5;
                dst[3] = has_alpha ? ((p & 0x8000) ? 255 : 0) : 255;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4: {
                uint16_t p = lduw_le_p(row + x * 2);
                dst[0] = geometry_expand_4((p >> 8) & 0x0f);
                dst[1] = geometry_expand_4((p >> 4) & 0x0f);
                dst[2] = geometry_expand_4(p & 0x0f);
                dst[3] = geometry_expand_4((p >> 12) & 0x0f);
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5: {
                uint16_t p = lduw_le_p(row + x * 2);
                dst[0] = geometry_expand_5((p >> 11) & 0x1f);
                dst[1] = geometry_expand_6((p >> 5) & 0x3f);
                dst[2] = geometry_expand_5(p & 0x1f);
                dst[3] = 255;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8: {
                const uint8_t *p = row + x * 4;
                dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0]; dst[3] = p[3];
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8: {
                const uint8_t *p = row + x * 4;
                dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0]; dst[3] = 255;
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8: {
                const uint8_t *p = row + x * 4;
                dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2]; dst[3] = p[3];
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8: {
                const uint8_t *p = row + x * 4;
                dst[0] = p[1]; dst[1] = p[2]; dst[2] = p[3]; dst[3] = p[0];
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8: {
                const uint8_t *p = row + x * 4;
                dst[0] = p[3]; dst[1] = p[2]; dst[2] = p[1]; dst[3] = p[0];
                break;
            }
            case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16: {
                uint16_t p = lduw_le_p(row + x * 2);
                uint8_t l = p >> 8;
                dst[0] = dst[1] = dst[2] = l;
                dst[3] = 255;
                break;
            }
            default:
                return false;
            }
        }
    }
    return true;
}

static bool geometry_decode_texture_face_rgba(NV2AState *d,
                                               const TextureShape *shape,
                                               const uint8_t *texture_data,
                                               const uint8_t *palette_data,
                                               uint8_t **out_rgba,
                                               unsigned int *out_width,
                                               unsigned int *out_height)
{
    if (!d || !shape || !texture_data || !out_rgba || !out_width ||
        !out_height || shape->dimensionality != 2 || shape->width == 0 ||
        shape->height == 0 || shape->color_format >= 66) {
        return false;
    }

    BasicColorFormatInfo f = kelvin_color_format_info_map[shape->color_format];
    if (f.bytes_per_pixel == 0 || f.depth) {
        return false;
    }

    unsigned int width = shape->width;
    unsigned int height = shape->height;
    unsigned int stored_width = width;
    unsigned int stored_height = height;
    if (!f.linear && shape->border) {
        stored_width = MAX(16u, stored_width * 2);
        stored_height = MAX(16u, stored_height * 2);
    }

    if (pgraph_is_texture_format_compressed(&d->pgraph,
                                            shape->color_format)) {
        enum S3TC_DECOMPRESS_FORMAT fmt;
        switch (shape->color_format) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
            fmt = S3TC_DECOMPRESS_FORMAT_DXT1;
            break;
        case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
            fmt = S3TC_DECOMPRESS_FORMAT_DXT3;
            break;
        case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
            fmt = S3TC_DECOMPRESS_FORMAT_DXT5;
            break;
        default:
            return false;
        }
        uint8_t *decoded = s3tc_decompress_2d(fmt, texture_data,
                                              stored_width, stored_height);
        if (!decoded) {
            return false;
        }
        if (shape->cubemap && stored_width != width &&
            stored_height != height && stored_width >= width + 8 &&
            stored_height >= height + 8) {
            uint8_t *cropped = g_malloc((size_t)width * height * 4);
            for (unsigned int y = 0; y < height; ++y) {
                memcpy(cropped + (size_t)y * width * 4,
                       decoded + ((size_t)(y + 4) * stored_width + 4) * 4,
                       (size_t)width * 4);
            }
            g_free(decoded);
            decoded = cropped;
        } else {
            width = stored_width;
            height = stored_height;
        }
        *out_rgba = decoded;
        *out_width = width;
        *out_height = height;
        return true;
    }

    unsigned int row_pitch = f.linear ? shape->pitch
                                      : stored_width * f.bytes_per_pixel;
    const uint8_t *pixel_data = texture_data;
    g_autofree uint8_t *unswizzled = NULL;
    if (!f.linear) {
        unswizzled = g_malloc((size_t)stored_height * row_pitch);
        unswizzle_rect(texture_data, stored_width, stored_height,
                       unswizzled, row_pitch, f.bytes_per_pixel);
        pixel_data = unswizzled;
    }

    size_t converted_size = 0;
    g_autofree uint8_t *converted = pgraph_convert_texture_data(
        *shape, pixel_data, palette_data, stored_width, stored_height, 1,
        row_pitch, 0, &converted_size);

    uint8_t *rgba = g_malloc((size_t)stored_width * stored_height * 4);
    bool ok = false;
    if (converted) {
        if (shape->color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
            ok = geometry_convert_packed_rgba(
                NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8, converted,
                stored_width, stored_height, stored_width * 4, rgba);
        } else if (shape->color_format ==
                       NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8 ||
                   shape->color_format ==
                       NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8) {
            if (converted_size >= (size_t)stored_width * stored_height * 4) {
                memcpy(rgba, converted,
                       (size_t)stored_width * stored_height * 4);
                ok = true;
            }
        } else if (shape->color_format ==
                   NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5) {
            if (converted_size >= (size_t)stored_width * stored_height * 3) {
                for (size_t i = 0, n = (size_t)stored_width * stored_height;
                     i < n; ++i) {
                    const int8_t *src = (const int8_t *)converted + i * 3;
                    /* Renderer storage is signed-normalized RGB. Visualize
                     * each signed byte component-for-component in PNG space. */
                    rgba[i * 4 + 0] = (uint8_t)((int)src[0] + 128);
                    rgba[i * 4 + 1] = (uint8_t)((int)src[1] + 128);
                    rgba[i * 4 + 2] = (uint8_t)((int)src[2] + 128);
                    rgba[i * 4 + 3] = 255;
                }
                ok = true;
            }
        }
    } else {
        ok = geometry_convert_packed_rgba(shape->color_format, pixel_data,
                                          stored_width, stored_height,
                                          row_pitch, rgba);
    }

    if (!ok) {
        g_free(rgba);
        return false;
    }

    *out_rgba = rgba;
    *out_width = stored_width;
    *out_height = stored_height;
    return true;
}

static size_t geometry_cubemap_face_span(const TextureShape *shape)
{
    if (!shape || !shape->cubemap) {
        return 0;
    }
    BasicColorFormatInfo f = kelvin_color_format_info_map[shape->color_format];
    unsigned int w = shape->width;
    unsigned int h = shape->height;
    if (!f.linear && shape->border) {
        w = MAX(16u, w * 2);
        h = MAX(16u, h * 2);
    }
    size_t length = 0;
    for (unsigned int level = 0; level < shape->levels; ++level) {
        w = MAX(w, 1u);
        h = MAX(h, 1u);
        if (shape->color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5) {
            unsigned int pw = (w + 3) & ~3u, ph = (h + 3) & ~3u;
            length += (size_t)(pw / 4) * (ph / 4) * 8;
        } else if (shape->color_format ==
                       NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ||
                   shape->color_format ==
                       NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8) {
            unsigned int pw = (w + 3) & ~3u, ph = (h + 3) & ~3u;
            length += (size_t)(pw / 4) * (ph / 4) * 16;
        } else {
            length += (size_t)w * h * f.bytes_per_pixel;
        }
        w /= 2;
        h /= 2;
    }
    return (length + NV2A_CUBEMAP_FACE_ALIGNMENT - 1) &
           ~(size_t)(NV2A_CUBEMAP_FACE_ALIGNMENT - 1);
}

static int geometry_gltf_wrap_mode(unsigned int address, bool *approx)
{
    if (approx) {
        *approx = false;
    }
    switch (address) {
    case NV_PGRAPH_TEXADDRESS0_ADDRU_WRAP:
        return 10497; /* REPEAT */
    case NV_PGRAPH_TEXADDRESS0_ADDRU_MIRROR:
        return 33648; /* MIRRORED_REPEAT */
    case NV_PGRAPH_TEXADDRESS0_ADDRU_CLAMP_TO_EDGE:
        return 33071; /* CLAMP_TO_EDGE */
    case NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER:
    case NV_PGRAPH_TEXADDRESS0_ADDRU_CLAMP_OGL:
        if (approx) {
            *approx = true;
        }
        return 33071; /* glTF has no border/legacy GL_CLAMP mode. */
    default:
        if (approx) {
            *approx = true;
        }
        return 10497;
    }
}

static int geometry_gltf_min_filter(unsigned int min_filter)
{
    static const int map[] = {
        9728, /* invalid/default -> NEAREST */
        9728, /* BOX_LOD0 -> NEAREST */
        9729, /* TENT_LOD0 -> LINEAR */
        9984, /* BOX_NEARESTLOD -> NEAREST_MIPMAP_NEAREST */
        9985, /* TENT_NEARESTLOD -> LINEAR_MIPMAP_NEAREST */
        9986, /* BOX_TENT_LOD -> NEAREST_MIPMAP_LINEAR */
        9987, /* TENT_TENT_LOD -> LINEAR_MIPMAP_LINEAR */
        9729, /* convolution approximation used by renderer */
    };
    return min_filter < ARRAY_SIZE(map) ? map[min_filter] : 9728;
}

static int geometry_gltf_mag_filter(unsigned int mag_filter)
{
    switch (mag_filter) {
    case 1: return 9728; /* NEAREST */
    case 2: return 9729; /* LINEAR */
    case 4: return 9729; /* convolution approximation */
    default: return 9729;
    }
}

static bool geometry_dump_texture_stage_locked(
    NV2AState *d, int slot, uint64_t draw_id, uint32_t frame_index,
    GeometryTextureExportInfo *info)
{
    PGRAPHState *pg = &d->pgraph;
    memset(info, 0, sizeof(*info));
    info->enabled = pgraph_is_texture_enabled(pg, slot);
    info->pixel_texture_mode =
        (pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG) >> (slot * 5)) & 0x1F;
    info->stage_active = pgraph_is_texture_stage_active(pg, slot);
    info->address_reg = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + slot * 4);
    info->filter_reg = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + slot * 4);
    info->border_color = pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + slot * 4);
    info->address_u = GET_MASK(info->address_reg, NV_PGRAPH_TEXADDRESS0_ADDRU);
    info->address_v = GET_MASK(info->address_reg, NV_PGRAPH_TEXADDRESS0_ADDRV);
    info->min_filter = GET_MASK(info->filter_reg, NV_PGRAPH_TEXFILTER0_MIN);
    info->mag_filter = GET_MASK(info->filter_reg, NV_PGRAPH_TEXFILTER0_MAG);
    uint32_t blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    uint32_t control0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    info->blend_enabled = (blend & NV_PGRAPH_BLEND_EN) != 0;
    info->alpha_test_enabled =
        (control0 & NV_PGRAPH_CONTROL_0_ALPHATESTENABLE) != 0;
    info->alpha_ref = GET_MASK(control0, NV_PGRAPH_CONTROL_0_ALPHAREF);
    info->alpha_func = GET_MASK(control0, NV_PGRAPH_CONTROL_0_ALPHAFUNC);
    if (!info->enabled || !g_geometry.dump_textures) {
        return false;
    }
    g_geometry.textures_referenced++;

    TextureShape shape = pgraph_get_texture_shape(pg, slot);
    info->cubemap = shape.cubemap;
    info->width = shape.width;
    info->height = shape.height;
    info->color_format = shape.color_format;
    info->linear = shape.color_format < 66 &&
                   kelvin_color_format_info_map[shape.color_format].linear;
    info->border = shape.border;
    if (info->linear) {
        switch (info->min_filter) {
        case NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD:
        case NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD:
            info->min_filter = NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0;
            break;
        case NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD:
        case NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD:
            info->min_filter = NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0;
            break;
        default:
            break;
        }
    }
    bool approx_u = false, approx_v = false;
    info->gltf_wrap_s = geometry_gltf_wrap_mode(info->address_u, &approx_u);
    info->gltf_wrap_t = geometry_gltf_wrap_mode(info->address_v, &approx_v);
    info->sampler_border_approx = approx_u || approx_v;
    info->gltf_min_filter = geometry_gltf_min_filter(info->min_filter);
    info->gltf_mag_filter = geometry_gltf_mag_filter(info->mag_filter);
    if (shape.color_format >= 66 || shape.width == 0 || shape.height == 0) {
        g_geometry.texture_dump_failures++;
        if (g_geometry.texture_manifest) {
            fprintf(g_geometry.texture_manifest,
                    "{\"type\":\"unsupported\",\"frame_index\":%u,"
                    "\"draw\":%" PRIu64 ",\"slot\":%d,"
                    "\"format\":%u,\"reason\":\"invalid_texture_shape\"}\n",
                    frame_index, draw_id, slot, shape.color_format);
        }
        return false;
    }
    if (shape.dimensionality != 2 ||
        kelvin_color_format_info_map[shape.color_format].depth) {
        /* Preserve the NV2A evidence instead of emitting a misleading color
         * PNG for depth/unsupported-dimensionality surfaces. These are known
         * non-color cases, not decoder failures. */
        if (g_geometry.texture_manifest) {
            fprintf(g_geometry.texture_manifest,
                    "{\"type\":\"unsupported\",\"frame_index\":%u,"
                    "\"draw\":%" PRIu64 ",\"slot\":%d,"
                    "\"format\":%u,\"width\":%u,\"height\":%u,"
                    "\"dimensionality\":%u,\"depth\":%s,"
                    "\"reason\":\"metadata_only_non_color\"}\n",
                    frame_index, draw_id, slot, shape.color_format,
                    shape.width, shape.height, shape.dimensionality,
                    kelvin_color_format_info_map[shape.color_format].depth
                        ? "true" : "false");
        }
        return false;
    }

    size_t texture_length = pgraph_get_texture_length(pg, &shape);
    hwaddr texture_offset = pgraph_get_texture_phys_addr(pg, slot);
    if (texture_offset >= memory_region_size(d->vram) ||
        texture_length > memory_region_size(d->vram) - texture_offset) {
        g_geometry.texture_dump_failures++;
        return false;
    }
    const uint8_t *texture_data = d->vram_ptr + texture_offset;

    size_t palette_length = 0;
    const uint8_t *palette_data = NULL;
    if (shape.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
        hwaddr palette_offset = pgraph_get_texture_palette_phys_addr_length(
            pg, slot, &palette_length);
        if (palette_offset >= memory_region_size(d->vram) ||
            palette_length > memory_region_size(d->vram) - palette_offset) {
            g_geometry.texture_dump_failures++;
            return false;
        }
        palette_data = d->vram_ptr + palette_offset;
    }

    uint64_t hash = fast_hash(texture_data, texture_length);
    if (palette_data && palette_length) {
        hash ^= fast_hash(palette_data, palette_length);
    }
    info->content_hash = hash;

    static const char *face_names[6] = {
        "posx", "negx", "posy", "negy", "posz", "negz",
    };
    int face_count = shape.cubemap ? 6 : 1;
    size_t face_span = shape.cubemap ? geometry_cubemap_face_span(&shape) : 0;
    bool any_dumped = false;

    for (int face = 0; face < face_count; ++face) {
        const char *face_name = shape.cubemap ? face_names[face] : NULL;
        g_autofree char *base_name = face_name
            ? g_strdup_printf("%016" PRIx64 "_fmt%02x_%ux%u_%s.png",
                              hash, shape.color_format, shape.width,
                              shape.height, face_name)
            : g_strdup_printf("%016" PRIx64 "_fmt%02x_%ux%u.png",
                              hash, shape.color_format, shape.width,
                              shape.height);
        g_autofree char *relative = g_strdup_printf("textures/%s", base_name);
        g_autofree char *key = g_strdup(relative);

        if (g_geometry.dumped_texture_keys &&
            g_hash_table_contains(g_geometry.dumped_texture_keys, key)) {
            any_dumped = true;
            if (!shape.cubemap && info->decoded_width == 0) {
                if (!info->linear && info->border) {
                    info->decoded_width = MAX(16u, info->width * 2);
                    info->decoded_height = MAX(16u, info->height * 2);
                } else {
                    info->decoded_width = info->width;
                    info->decoded_height = info->height;
                }
            }
            if (!info->file[0] && !shape.cubemap) {
                g_strlcpy(info->file, relative, sizeof(info->file));
            }
            continue;
        }
        if (g_geometry.failed_texture_keys &&
            g_hash_table_contains(g_geometry.failed_texture_keys, key)) {
            continue;
        }

        const uint8_t *face_data = texture_data + (shape.cubemap
            ? (size_t)face * face_span : 0);
        uint8_t *rgba = NULL;
        unsigned int decoded_w = 0, decoded_h = 0;
        if (!geometry_decode_texture_face_rgba(
                d, &shape, face_data, palette_data,
                &rgba, &decoded_w, &decoded_h)) {
            g_geometry.texture_dump_failures++;
            if (g_geometry.failed_texture_keys) {
                g_hash_table_add(g_geometry.failed_texture_keys, g_strdup(key));
            }
            continue;
        }

        g_autofree char *full_path =
            g_build_filename(g_geometry.output_path, relative, NULL);
        bool wrote = stbi_write_png(full_path, (int)decoded_w, (int)decoded_h,
                                    4, rgba, (int)decoded_w * 4) != 0;
        g_free(rgba);
        if (!wrote) {
            g_geometry.texture_dump_failures++;
            if (g_geometry.failed_texture_keys) {
                g_hash_table_add(g_geometry.failed_texture_keys, g_strdup(key));
            }
            continue;
        }

        if (g_geometry.dumped_texture_keys) {
            g_hash_table_add(g_geometry.dumped_texture_keys, g_strdup(key));
        }
        g_geometry.textures_dumped++;
        any_dumped = true;
        if (!shape.cubemap && info->decoded_width == 0) {
            info->decoded_width = decoded_w;
            info->decoded_height = decoded_h;
        }
        if (!info->file[0] && !shape.cubemap) {
            g_strlcpy(info->file, relative, sizeof(info->file));
        }
        if (g_geometry.texture_manifest) {
            fprintf(g_geometry.texture_manifest,
                    "{\"hash\":\"%016" PRIx64 "\",\"format\":%u,"
                    "\"width\":%u,\"height\":%u,\"cubemap\":%s,"
                    "\"face\":\"%s\",\"file\":\"%s\","
                    "\"nv2aAddress\":%u,\"nv2aFilter\":%u,"
                    "\"gltfWrapS\":%d,\"gltfWrapT\":%d,"
                    "\"gltfMinFilter\":%d,\"gltfMagFilter\":%d,"
                    "\"samplerApproximation\":%s,"
                    "\"source\":\"guest_vram\"}\n",
                    hash, shape.color_format, decoded_w, decoded_h,
                    shape.cubemap ? "true" : "false",
                    face_name ? face_name : "2d", relative,
                    info->address_reg, info->filter_reg,
                    info->gltf_wrap_s, info->gltf_wrap_t,
                    info->gltf_min_filter, info->gltf_mag_filter,
                    info->sampler_border_approx ? "true" : "false");
        }
    }

    info->dumped = any_dumped;
    if (g_geometry.texture_manifest && info->enabled) {
        fprintf(g_geometry.texture_manifest,
                "{\"type\":\"reference\",\"frame_index\":%u,"
                "\"draw\":%" PRIu64 ",\"slot\":%d,"
                "\"hash\":\"%016" PRIx64 "\",\"dumped\":%s,"
                "\"blendEnabled\":%s,\"alphaTestEnabled\":%s,"
                "\"alphaRef\":%u,\"alphaFunc\":%u}\n",
                frame_index, draw_id, slot, hash,
                any_dumped ? "true" : "false",
                info->blend_enabled ? "true" : "false",
                info->alpha_test_enabled ? "true" : "false",
                info->alpha_ref, info->alpha_func);
    }
    return any_dumped;
}


static unsigned int geometry_combiner_texture_score(PGRAPHState *pg, int slot)
{
    if (!pg || slot < 0 || slot >= NV2A_MAX_TEXTURES) {
        return 0;
    }

    const unsigned int target = PS_REGISTER_T0 + slot;
    unsigned int score = 0;
    unsigned int stages = pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL) & 0xff;
    stages = MIN(stages, 8u);

    /* Each combiner input byte stores the source register in its low nibble.
     * RGB references are more indicative of visible diffuse color than alpha,
     * and later stages receive a small preference because they are closer to
     * the final combiner result. This is deliberately a conservative direct-
     * reference heuristic rather than an invented full combiner dataflow. */
    for (unsigned int stage = 0; stage < stages; ++stage) {
        uint32_t color = pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORI0 + stage * 4);
        uint32_t alpha = pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAI0 + stage * 4);
        for (unsigned int input = 0; input < 4; ++input) {
            if (((color >> (input * 8)) & 0x0f) == target) {
                score += 8 + stage;
            }
            if (((alpha >> (input * 8)) & 0x0f) == target) {
                score += 2 + stage;
            }
        }
    }

    const uint32_t final0 = pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG0);
    const uint32_t final1 = pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG1);
    for (unsigned int input = 0; input < 4; ++input) {
        if (((final0 >> (input * 8)) & 0x0f) == target) {
            score += 24;
        }
        if (((final1 >> (input * 8)) & 0x0f) == target) {
            score += 24;
        }
    }
    return score;
}

static const char *geometry_pixel_texture_mode_name(unsigned int mode)
{
    switch (mode) {
    case PS_TEXTUREMODES_NONE: return "none";
    case PS_TEXTUREMODES_PROJECT2D: return "project2d";
    case PS_TEXTUREMODES_PROJECT3D: return "project3d";
    case PS_TEXTUREMODES_CUBEMAP: return "cubemap";
    case PS_TEXTUREMODES_PASSTHRU: return "passthru";
    case PS_TEXTUREMODES_CLIPPLANE: return "clipplane";
    case PS_TEXTUREMODES_BUMPENVMAP: return "bumpenvmap";
    case PS_TEXTUREMODES_BUMPENVMAP_LUM: return "bumpenvmap_lum";
    case PS_TEXTUREMODES_BRDF: return "brdf";
    case PS_TEXTUREMODES_DOT_ST: return "dot_st";
    case PS_TEXTUREMODES_DOT_ZW: return "dot_zw";
    case PS_TEXTUREMODES_DOT_RFLCT_DIFF: return "dot_reflect_diff";
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC: return "dot_reflect_spec";
    case PS_TEXTUREMODES_DOT_STR_3D: return "dot_str_3d";
    case PS_TEXTUREMODES_DOT_STR_CUBE: return "dot_str_cube";
    case PS_TEXTUREMODES_DPNDNT_AR: return "dependent_ar";
    case PS_TEXTUREMODES_DPNDNT_GB: return "dependent_gb";
    case PS_TEXTUREMODES_DOTPRODUCT: return "dotproduct";
    case PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST: return "dot_reflect_spec_const";
    default: return "unknown";
    }
}

static bool geometry_texture_stage_obj_mappable(
    const GeometryTextureExportInfo *info)
{
    if (!info || !info->enabled || !info->stage_active || !info->dumped ||
        info->cubemap || !info->file[0] || info->width == 0 ||
        info->height == 0) {
        return false;
    }

    /* OBJ/MTL has one ordinary 2D UV set and cannot represent dependent,
     * bump, 3D, cube, shadow/dot-product, or passthrough stage semantics.
     * Mapping those as map_Kd was the major V3 correctness bug: a perfectly
     * valid dumped image could be attached to geometry using the wrong
     * coordinate-generation rule. Only a direct projected 2D sample is safe
     * enough to auto-bind. Everything else remains in JSON/manifest metadata. */
    return info->pixel_texture_mode == PS_TEXTUREMODES_PROJECT2D;
}

static bool geometry_post_vsh_coord_to_obj_uv(
    const GeometryTextureExportInfo *info, const float coord[4], GeometryUV *uv)
{
    if (!info || !coord || !uv || !geometry_texture_stage_obj_mappable(info)) {
        return false;
    }

    float q = coord[3];
    if (!isfinite(q) || fabsf(q) < 1.0e-20f) {
        return false;
    }

    /* NV2A PS_TEXTUREMODES_PROJECT2D feeds pT.xyw to textureProj, so the
     * actual sampling coordinate is S/Q,T/Q. V3 exported the raw vertex input
     * S,T and therefore ignored both Q projection and all vertex-shader
     * texgen/texture-matrix work. */
    float u;
    float v;
    if (info->linear) {
        /* Rectangle/linear Xbox textures use texel-space coordinates. Xemu's
         * pixel shader normalizes them by textureSize before textureProj. */
        u = coord[0] / ((float)info->width * q);
        v = coord[1] / ((float)info->height * q);
    } else if (info->border && info->decoded_width && info->decoded_height) {
        /* Border adjustment happens before textureProj in Xemu, so the +4
         * texel offset is divided by Q as well. */
        u = (coord[0] * (float)info->width + 4.0f) /
            ((float)info->decoded_width * q);
        v = (coord[1] * (float)info->height + 4.0f) /
            ((float)info->decoded_height * q);
    } else {
        u = coord[0] / q;
        v = coord[1] / q;
    }

    if (!isfinite(u) || !isfinite(v)) {
        return false;
    }
    uv->u = u;
    uv->v = v;
    uv->valid = true;
    return true;
}

static bool geometry_build_material_uvs(
    const GArray *vertices, int stage,
    const GeometryTextureExportInfo *info, GArray *uvs)
{
    if (!vertices || !uvs || stage < 0 || stage >= NV2A_MAX_TEXTURES ||
        !geometry_texture_stage_obj_mappable(info)) {
        return false;
    }
    bool q_ref_set = false;
    float q_ref = 1.0f;
    for (guint i = 0; i < vertices->len; ++i) {
        const GeometryVertex *v = &g_array_index(vertices, GeometryVertex, i);
        if (!v->post_vsh_texcoord_valid[stage]) {
            g_array_set_size(uvs, 0);
            return false;
        }
        float q = v->post_vsh_texcoord[stage][3];
        if (!q_ref_set) {
            q_ref = q;
            q_ref_set = true;
        } else {
            float tol = 1.0e-5f * MAX(1.0f, MAX(fabsf(q_ref), fabsf(q)));
            if (fabsf(q - q_ref) > tol) {
                /* OBJ has no projective Q coordinate. Baking S/Q,T/Q at each
                 * vertex would interpolate differently when Q varies, so do
                 * not advertise a knowingly-wrong automatic material. */
                g_array_set_size(uvs, 0);
                return false;
            }
        }
        GeometryUV uv = { 0 };
        if (!geometry_post_vsh_coord_to_obj_uv(
                info, v->post_vsh_texcoord[stage], &uv)) {
            g_array_set_size(uvs, 0);
            return false;
        }
        g_array_append_val(uvs, uv);
    }
    return uvs->len == vertices->len;
}

static uint64_t geometry_write_face(FILE *obj, uint64_t a, uint64_t b,
                                    uint64_t c, uint32_t source_a,
                                    uint32_t source_b, uint32_t source_c,
                                    bool has_uv, bool has_normal,
                                    bool reverse_winding)
{
    /* Indexed strips commonly repeat a source index to emit a degenerate
     * primitive and restart/connect strips. OBJ positions are duplicated per
     * occurrence, so detect degeneracy from the original Xbox indices. */
    if (source_a == source_b || source_b == source_c || source_a == source_c) {
        return 0;
    }
    if (reverse_winding) {
        uint64_t tmp_v = b;
        b = c;
        c = tmp_v;
        uint32_t tmp_s = source_b;
        source_b = source_c;
        source_c = tmp_s;
    }
#define OBJ_REF(v) ((unsigned long long)(v))
    if (has_uv && has_normal) {
        fprintf(obj, "f %llu/%llu/%llu %llu/%llu/%llu %llu/%llu/%llu\n",
                OBJ_REF(a), OBJ_REF(a), OBJ_REF(a),
                OBJ_REF(b), OBJ_REF(b), OBJ_REF(b),
                OBJ_REF(c), OBJ_REF(c), OBJ_REF(c));
    } else if (has_uv) {
        fprintf(obj, "f %llu/%llu %llu/%llu %llu/%llu\n",
                OBJ_REF(a), OBJ_REF(a), OBJ_REF(b), OBJ_REF(b),
                OBJ_REF(c), OBJ_REF(c));
    } else if (has_normal) {
        fprintf(obj, "f %llu//%llu %llu//%llu %llu//%llu\n",
                OBJ_REF(a), OBJ_REF(a), OBJ_REF(b), OBJ_REF(b),
                OBJ_REF(c), OBJ_REF(c));
    } else {
        fprintf(obj, "f %llu %llu %llu\n", OBJ_REF(a), OBJ_REF(b), OBJ_REF(c));
    }
#undef OBJ_REF
    return 1;
}

static uint64_t geometry_write_segment(FILE *obj, uint32_t primitive,
                                       uint64_t base,
                                       const GeometryVertex *vertices,
                                       uint32_t first, uint32_t count,
                                       bool has_uv, bool has_normal,
                                       bool reverse_winding)
{
    uint64_t prims = 0;
#define V(i) (base + (uint64_t)(i) + 1)
#define S(i) (vertices[first + (uint32_t)(i)].source_index)
    switch (primitive) {
    case PRIM_TYPE_POINTS:
        for (uint32_t i = 0; i < count; ++i) {
            fprintf(obj, "p %llu\n", (unsigned long long)V(i));
            prims++;
        }
        break;
    case PRIM_TYPE_LINES:
        for (uint32_t i = 0; i + 1 < count; i += 2) {
            fprintf(obj, "l %llu %llu\n", (unsigned long long)V(i),
                    (unsigned long long)V(i + 1));
            prims++;
        }
        break;
    case PRIM_TYPE_LINE_LOOP:
        if (count > 1) {
            fprintf(obj, "l");
            for (uint32_t i = 0; i < count; ++i) {
                fprintf(obj, " %llu", (unsigned long long)V(i));
            }
            fprintf(obj, " %llu\n", (unsigned long long)V(0));
            prims = count;
        }
        break;
    case PRIM_TYPE_LINE_STRIP:
        if (count > 1) {
            fprintf(obj, "l");
            for (uint32_t i = 0; i < count; ++i) {
                fprintf(obj, " %llu", (unsigned long long)V(i));
            }
            fprintf(obj, "\n");
            prims = count - 1;
        }
        break;
    case PRIM_TYPE_TRIANGLES:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            prims += geometry_write_face(obj, V(i), V(i + 1), V(i + 2),
                                         S(i), S(i + 1), S(i + 2),
                                         has_uv, has_normal, reverse_winding);
        }
        break;
    case PRIM_TYPE_TRIANGLE_STRIP:
        for (uint32_t i = 2; i < count; ++i) {
            uint64_t a = V(i - 2), b = V(i - 1), c = V(i);
            if (i & 1) {
                uint64_t tmp = a;
                a = b;
                b = tmp;
            }
            prims += geometry_write_face(
                obj, a, b, c,
                (i & 1) ? S(i - 1) : S(i - 2),
                (i & 1) ? S(i - 2) : S(i - 1), S(i),
                has_uv, has_normal, reverse_winding);
        }
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
        for (uint32_t i = 2; i < count; ++i) {
            prims += geometry_write_face(obj, V(0), V(i - 1), V(i),
                                         S(0), S(i - 1), S(i),
                                         has_uv, has_normal, reverse_winding);
        }
        break;
    case PRIM_TYPE_QUADS:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            /* Match xemu's NV2A geometry shader split exactly. */
            prims += geometry_write_face(obj, V(i + 1), V(i + 2), V(i),
                                         S(i + 1), S(i + 2), S(i),
                                         has_uv, has_normal, reverse_winding);
            prims += geometry_write_face(obj, V(i + 2), V(i + 3), V(i),
                                         S(i + 2), S(i + 3), S(i),
                                         has_uv, has_normal, reverse_winding);
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            prims += geometry_write_face(obj, V(i), V(i + 1), V(i + 2),
                                         S(i), S(i + 1), S(i + 2),
                                         has_uv, has_normal, reverse_winding);
            prims += geometry_write_face(obj, V(i + 2), V(i + 1), V(i + 3),
                                         S(i + 2), S(i + 1), S(i + 3),
                                         has_uv, has_normal, reverse_winding);
        }
        break;
    default:
        break;
    }
#undef S
#undef V
    return prims;
}


/* ------------------------------------------------------------------------- */
/* glTF 2.0 streaming exporter                                               */
/* ------------------------------------------------------------------------- */

static bool geometry_gltf_write_u32(FILE *f, uint32_t value)
{
    uint32_t le = cpu_to_le32(value);
    return fwrite(&le, 1, sizeof(le), f) == sizeof(le);
}

static bool geometry_gltf_write_float(FILE *f, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return geometry_gltf_write_u32(f, bits);
}

static bool geometry_gltf_align4(GeometryGltfExport *out)
{
    if (!out || !out->bin) {
        return false;
    }
    while (out->byte_length & 3u) {
        if (fputc(0, out->bin) == EOF) {
            return false;
        }
        out->byte_length++;
    }
    return true;
}

static bool geometry_gltf_append_index(GArray *indices, uint32_t value)
{
    if (!indices) {
        return false;
    }
    g_array_append_val(indices, value);
    return true;
}

static bool geometry_gltf_triangle_degenerate(const GeometryVertex *vertices,
                                               uint32_t first,
                                               uint32_t a, uint32_t b,
                                               uint32_t c)
{
    const uint32_t sa = vertices[first + a].source_index;
    const uint32_t sb = vertices[first + b].source_index;
    const uint32_t sc = vertices[first + c].source_index;
    return sa == sb || sb == sc || sc == sa;
}

static void geometry_gltf_add_triangle(GArray *indices,
                                       const GeometryVertex *vertices,
                                       uint32_t first,
                                       uint32_t a, uint32_t b, uint32_t c)
{
    if (geometry_gltf_triangle_degenerate(vertices, first, a, b, c)) {
        return;
    }
    geometry_gltf_append_index(indices, a);
    geometry_gltf_append_index(indices, b);
    geometry_gltf_append_index(indices, c);
}

static bool geometry_gltf_build_indices(uint32_t primitive,
                                        const GeometryVertex *vertices,
                                        uint32_t first, uint32_t count,
                                        GArray *indices,
                                        uint32_t *gltf_mode)
{
    if (!vertices || !indices || !gltf_mode || count == 0) {
        return false;
    }
    *gltf_mode = 4; /* TRIANGLES */
    switch (primitive) {
    case PRIM_TYPE_POINTS:
        *gltf_mode = 0;
        for (uint32_t i = 0; i < count; ++i) {
            geometry_gltf_append_index(indices, i);
        }
        break;
    case PRIM_TYPE_LINES:
        *gltf_mode = 1;
        for (uint32_t i = 0; i + 1 < count; i += 2) {
            geometry_gltf_append_index(indices, i);
            geometry_gltf_append_index(indices, i + 1);
        }
        break;
    case PRIM_TYPE_LINE_LOOP:
        *gltf_mode = 2;
        for (uint32_t i = 0; i < count; ++i) {
            geometry_gltf_append_index(indices, i);
        }
        break;
    case PRIM_TYPE_LINE_STRIP:
        *gltf_mode = 3;
        for (uint32_t i = 0; i < count; ++i) {
            geometry_gltf_append_index(indices, i);
        }
        break;
    case PRIM_TYPE_TRIANGLES:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            geometry_gltf_add_triangle(indices, vertices, first,
                                       i, i + 1, i + 2);
        }
        break;
    case PRIM_TYPE_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < count; ++i) {
            if (i & 1u) {
                geometry_gltf_add_triangle(indices, vertices, first,
                                           i + 1, i, i + 2);
            } else {
                geometry_gltf_add_triangle(indices, vertices, first,
                                           i, i + 1, i + 2);
            }
        }
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
        for (uint32_t i = 1; i + 1 < count; ++i) {
            geometry_gltf_add_triangle(indices, vertices, first,
                                       0, i, i + 1);
        }
        break;
    case PRIM_TYPE_QUADS:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            geometry_gltf_add_triangle(indices, vertices, first,
                                       i, i + 1, i + 2);
            geometry_gltf_add_triangle(indices, vertices, first,
                                       i + 2, i + 3, i);
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < count; i += 2) {
            geometry_gltf_add_triangle(indices, vertices, first,
                                       i, i + 1, i + 2);
            geometry_gltf_add_triangle(indices, vertices, first,
                                       i + 2, i + 1, i + 3);
        }
        break;
    default:
        return false;
    }
    return indices->len != 0;
}

static bool geometry_gltf_append_primitive(
    GeometryGltfExport *out, const GArray *vertices,
    const GeometrySegment *seg, uint32_t segment_index,
    uint32_t primitive_mode, GeometrySourceKind source,
    uint32_t frame_index, uint64_t draw_id, bool has_normal,
    bool material_mapped, const GArray *material_uvs,
    int material_texture_slot, const GeometryTextureExportInfo *texture_info)
{
    if (!out || !out->enabled || !out->bin || !out->primitives || !vertices ||
        !seg || seg->vertex_count == 0 ||
        seg->first_vertex + seg->vertex_count > vertices->len) {
        return false;
    }

    const GeometryVertex *all = &g_array_index(vertices, GeometryVertex, 0);
    g_autoptr(GArray) indices = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    uint32_t gltf_mode = 4;
    if (!geometry_gltf_build_indices(primitive_mode, all, seg->first_vertex,
                                     seg->vertex_count, indices, &gltf_mode)) {
        return false;
    }
    if (!out->native_basis && gltf_mode == 4) {
        /* Reflecting Z changes handedness, so every triangle must reverse
         * winding to preserve the original front face. */
        for (guint i = 0; i + 2 < indices->len; i += 3) {
            uint32_t b = g_array_index(indices, uint32_t, i + 1);
            uint32_t c = g_array_index(indices, uint32_t, i + 2);
            g_array_index(indices, uint32_t, i + 1) = c;
            g_array_index(indices, uint32_t, i + 2) = b;
        }
    }

    GeometryGltfPrimitive rec;
    memset(&rec, 0, sizeof(rec));
    rec.frame_index = frame_index;
    rec.draw_id = draw_id;
    rec.segment_index = segment_index;
    rec.source_kind = (uint32_t)source;
    rec.xbox_primitive_mode = primitive_mode;
    rec.gltf_mode = gltf_mode;
    rec.vertex_count = seg->vertex_count;
    rec.index_count = indices->len;
    rec.has_normal = has_normal;
    rec.has_uv = material_mapped && material_uvs &&
                 material_uvs->len == vertices->len;
    rec.material_texture_slot = rec.has_uv ? material_texture_slot : -1;
    rec.position_accessor = rec.normal_accessor = rec.uv_accessor =
        rec.index_accessor = rec.material_index = rec.mesh_index = -1;
    if (rec.has_uv && texture_info && texture_info->file[0]) {
        g_strlcpy(rec.texture_file, texture_info->file,
                  sizeof(rec.texture_file));
        rec.gltf_mag_filter = texture_info->gltf_mag_filter;
        rec.gltf_min_filter = texture_info->gltf_min_filter;
        rec.gltf_wrap_s = texture_info->gltf_wrap_s;
        rec.gltf_wrap_t = texture_info->gltf_wrap_t;
        rec.sampler_border_approx = texture_info->sampler_border_approx;
        rec.blend_enabled = texture_info->blend_enabled;
        rec.alpha_test_enabled = texture_info->alpha_test_enabled;
        rec.alpha_ref = texture_info->alpha_ref;
        rec.alpha_func = texture_info->alpha_func;
    }

    if (!geometry_gltf_align4(out)) {
        return false;
    }
    rec.position_offset = out->byte_length;
    for (uint32_t i = 0; i < seg->vertex_count; ++i) {
        const GeometryVertex *v = &all[seg->first_vertex + i];
        float p[3] = {
            v->position[0] * g_geometry.export_scale,
            v->position[1] * g_geometry.export_scale,
            (out->native_basis ? v->position[2] : -v->position[2]) *
                g_geometry.export_scale,
        };
        for (int c = 0; c < 3; ++c) {
            if (i == 0 || p[c] < rec.position_min[c]) rec.position_min[c] = p[c];
            if (i == 0 || p[c] > rec.position_max[c]) rec.position_max[c] = p[c];
            if (!geometry_gltf_write_float(out->bin, p[c])) return false;
            out->byte_length += 4;
        }
    }

    if (rec.has_normal) {
        if (!geometry_gltf_align4(out)) return false;
        rec.normal_offset = out->byte_length;
        for (uint32_t i = 0; i < seg->vertex_count; ++i) {
            const GeometryVertex *v = &all[seg->first_vertex + i];
            float n[3] = { v->normal[0], v->normal[1],
                           out->native_basis ? v->normal[2] : -v->normal[2] };
            float len2 = n[0]*n[0] + n[1]*n[1] + n[2]*n[2];
            if (len2 > 0.0f && isfinite(len2)) {
                float inv = 1.0f / sqrtf(len2);
                n[0] *= inv; n[1] *= inv; n[2] *= inv;
            } else {
                n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f;
            }
            for (int c = 0; c < 3; ++c) {
                if (!geometry_gltf_write_float(out->bin, n[c])) return false;
                out->byte_length += 4;
            }
        }
    }

    if (rec.has_uv) {
        if (!geometry_gltf_align4(out)) return false;
        rec.uv_offset = out->byte_length;
        for (uint32_t i = 0; i < seg->vertex_count; ++i) {
            const GeometryUV *uv = &g_array_index(
                material_uvs, GeometryUV, seg->first_vertex + i);
            if (!geometry_gltf_write_float(out->bin, uv->u) ||
                !geometry_gltf_write_float(out->bin, uv->v)) {
                return false;
            }
            out->byte_length += 8;
        }
    }

    if (!geometry_gltf_align4(out)) return false;
    rec.index_offset = out->byte_length;
    for (guint i = 0; i < indices->len; ++i) {
        uint32_t idx = g_array_index(indices, uint32_t, i);
        if (!geometry_gltf_write_u32(out->bin, idx)) return false;
        out->byte_length += 4;
    }

    g_array_append_val(out->primitives, rec);
    return true;
}

static void geometry_gltf_assign_indices(GeometryGltfExport *out)
{
    int accessor = 0;
    int mesh_index = -1;
    uint64_t last_draw = UINT64_MAX;
    uint32_t last_frame = UINT32_MAX;
    for (guint i = 0; i < out->primitives->len; ++i) {
        GeometryGltfPrimitive *p = &g_array_index(
            out->primitives, GeometryGltfPrimitive, i);
        p->position_accessor = accessor++;
        if (p->has_normal) p->normal_accessor = accessor++;
        if (p->has_uv) p->uv_accessor = accessor++;
        p->index_accessor = accessor++;
        if (p->draw_id != last_draw || p->frame_index != last_frame) {
            mesh_index++;
            last_draw = p->draw_id;
            last_frame = p->frame_index;
        }
        p->mesh_index = mesh_index;
    }
}

static void geometry_gltf_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else fputc(*p, f);
            break;
        }
    }
    fputc('"', f);
}

typedef struct GeometryGltfMaterialDef {
    char texture_file[256];
    int mag_filter;
    int min_filter;
    int wrap_s;
    int wrap_t;
    bool border_approx;
    bool blend_enabled;
    bool alpha_test_enabled;
    unsigned int alpha_ref;
    unsigned int alpha_func;
} GeometryGltfMaterialDef;

static bool geometry_gltf_write_json(GeometryGltfExport *out)
{
    if (!out || !out->enabled || !out->primitives || !out->json_path[0] ||
        !out->bin_uri[0]) {
        return true;
    }
    if (out->bin) fflush(out->bin);
    geometry_gltf_assign_indices(out);

    g_autoptr(GHashTable) material_indices =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GArray) material_defs =
        g_array_new(FALSE, FALSE, sizeof(GeometryGltfMaterialDef));
    for (guint i = 0; i < out->primitives->len; ++i) {
        GeometryGltfPrimitive *p = &g_array_index(
            out->primitives, GeometryGltfPrimitive, i);
        p->material_index = 0; /* double-sided untextured default */
        if (!p->has_uv || !p->texture_file[0]) {
            continue;
        }

        g_autofree char *key = g_strdup_printf(
            "%s|mag=%d|min=%d|s=%d|t=%d|border=%d|blend=%d|atest=%d|aref=%u|afunc=%u",
            p->texture_file, p->gltf_mag_filter, p->gltf_min_filter,
            p->gltf_wrap_s, p->gltf_wrap_t, p->sampler_border_approx,
            p->blend_enabled, p->alpha_test_enabled, p->alpha_ref, p->alpha_func);
        gpointer found = g_hash_table_lookup(material_indices, key);
        int material_def_index;
        if (found) {
            material_def_index = GPOINTER_TO_INT(found) - 1;
        } else {
            GeometryGltfMaterialDef def = { 0 };
            g_strlcpy(def.texture_file, p->texture_file, sizeof(def.texture_file));
            def.mag_filter = p->gltf_mag_filter;
            def.min_filter = p->gltf_min_filter;
            def.wrap_s = p->gltf_wrap_s;
            def.wrap_t = p->gltf_wrap_t;
            def.border_approx = p->sampler_border_approx;
            def.blend_enabled = p->blend_enabled;
            def.alpha_test_enabled = p->alpha_test_enabled;
            def.alpha_ref = p->alpha_ref;
            def.alpha_func = p->alpha_func;
            material_def_index = (int)material_defs->len;
            g_array_append_val(material_defs, def);
            g_hash_table_insert(material_indices, g_strdup(key),
                                GINT_TO_POINTER(material_def_index + 1));
        }
        p->material_index = material_def_index + 1;
    }

    FILE *f = g_fopen(out->json_path, "wb");
    if (!f) return false;

    if (out->primitives->len == 0) {
        fprintf(f,
                "{\n  \"asset\":{\"version\":\"2.0\",\"generator\":"
                "\"xemu custom fork NV2A Geometry Dumper V6\"},\n"
                "  \"extras\":{\"xemuNativeCoordinateBasis\":%s,"
                "\"exportScale\":%.9g,\"placedScene\":%s},\n"
                "  \"scenes\":[{\"name\":\"xemu_capture\",\"nodes\":[]}],\n"
                "  \"scene\":0\n}\n",
                out->native_basis ? "true" : "false",
                g_geometry.export_scale, out->placed ? "true" : "false");
        bool ok = !ferror(f);
        fclose(f);
        return ok;
    }

    fprintf(f,
            "{\n  \"asset\":{\"version\":\"2.0\",\"generator\":"
            "\"xemu custom fork NV2A Geometry Dumper V6\"},\n"
            "  \"extras\":{\"xemuNativeCoordinateBasis\":%s,"
            "\"exportScale\":%.9g,\"placedScene\":%s,"
            "\"coordinateConversion\":\"%s\"},\n",
            out->native_basis ? "true" : "false",
            g_geometry.export_scale, out->placed ? "true" : "false",
            out->native_basis ? "none" : "x=x,y=y,z=-z; triangle winding reversed");

    fprintf(f, "  \"buffers\":[{\"uri\":");
    geometry_gltf_json_string(f, out->bin_uri);
    fprintf(f, ",\"byteLength\":%" PRIu64 "}],\n", out->byte_length);

    /* One bufferView per accessor keeps the streaming writer simple and makes
     * the binary layout easy to audit. */
    fprintf(f, "  \"bufferViews\":[\n");
    int view_index = 0;
    for (guint i = 0; i < out->primitives->len; ++i) {
        GeometryGltfPrimitive *p = &g_array_index(out->primitives, GeometryGltfPrimitive, i);
#define VIEW(off,len,target) do { \
        if (view_index++) fputs(",\n", f); \
        fprintf(f, "    {\"buffer\":0,\"byteOffset\":%" PRIu64 \
                   ",\"byteLength\":%u,\"target\":%u}", \
                (uint64_t)(off), (unsigned)(len), (unsigned)(target)); \
    } while (0)
        VIEW(p->position_offset, p->vertex_count * 12u, 34962);
        if (p->has_normal) VIEW(p->normal_offset, p->vertex_count * 12u, 34962);
        if (p->has_uv) VIEW(p->uv_offset, p->vertex_count * 8u, 34962);
        VIEW(p->index_offset, p->index_count * 4u, 34963);
#undef VIEW
    }
    fprintf(f, "\n  ],\n");

    fprintf(f, "  \"accessors\":[\n");
    int acc_index = 0;
    int bv_index = 0;
    for (guint i = 0; i < out->primitives->len; ++i) {
        GeometryGltfPrimitive *p = &g_array_index(out->primitives, GeometryGltfPrimitive, i);
#define ACC_SEP() do { if (acc_index++) fputs(",\n", f); } while (0)
        ACC_SEP();
        fprintf(f, "    {\"bufferView\":%d,\"componentType\":5126,\"count\":%u,"
                   "\"type\":\"VEC3\",\"min\":[%.9g,%.9g,%.9g],"
                   "\"max\":[%.9g,%.9g,%.9g]}",
                bv_index++, p->vertex_count,
                p->position_min[0], p->position_min[1], p->position_min[2],
                p->position_max[0], p->position_max[1], p->position_max[2]);
        if (p->has_normal) {
            ACC_SEP();
            fprintf(f, "    {\"bufferView\":%d,\"componentType\":5126,"
                       "\"count\":%u,\"type\":\"VEC3\"}",
                    bv_index++, p->vertex_count);
        }
        if (p->has_uv) {
            ACC_SEP();
            fprintf(f, "    {\"bufferView\":%d,\"componentType\":5126,"
                       "\"count\":%u,\"type\":\"VEC2\"}",
                    bv_index++, p->vertex_count);
        }
        ACC_SEP();
        fprintf(f, "    {\"bufferView\":%d,\"componentType\":5125,"
                   "\"count\":%u,\"type\":\"SCALAR\"}",
                bv_index++, p->index_count);
#undef ACC_SEP
    }
    fprintf(f, "\n  ],\n");

    if (material_defs->len) {
        fprintf(f, "  \"samplers\":[");
        for (guint i = 0; i < material_defs->len; ++i) {
            const GeometryGltfMaterialDef *def =
                &g_array_index(material_defs, GeometryGltfMaterialDef, i);
            if (i) fputc(',', f);
            fprintf(f, "{\"magFilter\":%d,\"minFilter\":%d,"
                       "\"wrapS\":%d,\"wrapT\":%d",
                    def->mag_filter, def->min_filter, def->wrap_s, def->wrap_t);
            if (def->border_approx) {
                fputs(",\"extras\":{\"xemuBorderClampApproximation\":true}", f);
            }
            fputc('}', f);
        }
        fprintf(f, "],\n  \"images\":[");
        for (guint i = 0; i < material_defs->len; ++i) {
            const GeometryGltfMaterialDef *def =
                &g_array_index(material_defs, GeometryGltfMaterialDef, i);
            if (i) fputc(',', f);
            fputs("{\"uri\":", f);
            geometry_gltf_json_string(f, def->texture_file);
            fputc('}', f);
        }
        fprintf(f, "],\n  \"textures\":[");
        for (guint i = 0; i < material_defs->len; ++i) {
            if (i) fputc(',', f);
            fprintf(f, "{\"sampler\":%u,\"source\":%u}", i, i);
        }
        fprintf(f, "],\n");
    }

    fprintf(f, "  \"materials\":["
               "{\"name\":\"xemu_default\",\"doubleSided\":true,"
               "\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],"
               "\"metallicFactor\":0,\"roughnessFactor\":1}}" );
    for (guint i = 0; i < material_defs->len; ++i) {
        const GeometryGltfMaterialDef *def =
            &g_array_index(material_defs, GeometryGltfMaterialDef, i);
        fprintf(f, ",{\"name\":\"xemu_tex_%u\",\"doubleSided\":true,"
                   "\"pbrMetallicRoughness\":{\"baseColorTexture\":{"
                   "\"index\":%u,\"texCoord\":0},\"metallicFactor\":0,"
                   "\"roughnessFactor\":1}", i, i);
        if (def->blend_enabled) {
            fputs(",\"alphaMode\":\"BLEND\"", f);
        } else if (def->alpha_test_enabled) {
            fprintf(f, ",\"alphaMode\":\"MASK\",\"alphaCutoff\":%.9g",
                    (double)def->alpha_ref / 255.0);
        } else {
            fputs(",\"alphaMode\":\"OPAQUE\"", f);
        }
        fputs(",\"extras\":{\"sourceImage\":", f);
        geometry_gltf_json_string(f, def->texture_file);
        fprintf(f, ",\"xboxBlendEnabled\":%s,\"xboxAlphaTestEnabled\":%s,"
                   "\"xboxAlphaRef\":%u,\"xboxAlphaFunc\":%u}}",
                def->blend_enabled ? "true" : "false",
                def->alpha_test_enabled ? "true" : "false",
                def->alpha_ref, def->alpha_func);
    }
    fprintf(f, "],\n");

    /* Meshes are grouped by original NV2A draw; each draw-array/inline segment
     * becomes a glTF primitive within that mesh. */
    fprintf(f, "  \"meshes\":[\n");
    int current_mesh = -1;
    bool first_mesh = true;
    bool first_prim = true;
    for (guint i = 0; i < out->primitives->len; ++i) {
        GeometryGltfPrimitive *p = &g_array_index(out->primitives, GeometryGltfPrimitive, i);
        if (p->mesh_index != current_mesh) {
            if (current_mesh >= 0) fputs("]}", f);
            if (!first_mesh) fputs(",\n", f);
            first_mesh = false;
            current_mesh = p->mesh_index;
            first_prim = true;
            fprintf(f, "    {\"name\":\"frame_%04u_draw_%06" PRIu64
                       "\",\"primitives\":[", p->frame_index, p->draw_id);
        }
        if (!first_prim) fputc(',', f);
        first_prim = false;
        fprintf(f, "{\"attributes\":{\"POSITION\":%d",
                p->position_accessor);
        if (p->has_normal) fprintf(f, ",\"NORMAL\":%d", p->normal_accessor);
        if (p->has_uv) fprintf(f, ",\"TEXCOORD_0\":%d", p->uv_accessor);
        fprintf(f, "},\"indices\":%d,\"material\":%d,\"mode\":%u,"
                   "\"extras\":{\"xemuFrame\":%u,\"xemuDraw\":%" PRIu64
                   ",\"xemuSegment\":%u,\"xboxPrimitiveMode\":%u,"
                   "\"xboxPrimitiveName\":",
                p->index_accessor, p->material_index, p->gltf_mode,
                p->frame_index, p->draw_id, p->segment_index,
                p->xbox_primitive_mode);
        geometry_gltf_json_string(f, geometry_primitive_name(p->xbox_primitive_mode));
        fprintf(f, ",\"xboxSource\":");
        geometry_gltf_json_string(f, geometry_source_name((GeometrySourceKind)p->source_kind));
        fprintf(f, ",\"textureStage\":%d}}", p->material_texture_slot);
    }
    if (current_mesh >= 0) fputs("]}", f);
    fprintf(f, "\n  ],\n");

    /* One node per draw mesh. */
    int mesh_count = current_mesh + 1;
    fprintf(f, "  \"nodes\":[");
    for (int m = 0; m < mesh_count; ++m) {
        if (m) fputc(',', f);
        const GeometryGltfPrimitive *first = NULL;
        for (guint i = 0; i < out->primitives->len; ++i) {
            const GeometryGltfPrimitive *p = &g_array_index(out->primitives, GeometryGltfPrimitive, i);
            if (p->mesh_index == m) { first = p; break; }
        }
        fprintf(f, "{\"mesh\":%d,\"name\":\"frame_%04u_draw_%06" PRIu64 "\"}",
                m, first ? first->frame_index : 0,
                first ? first->draw_id : 0);
    }
    fprintf(f, "],\n  \"scenes\":[{\"name\":\"xemu_capture\",\"nodes\":[");
    for (int m = 0; m < mesh_count; ++m) {
        if (m) fputc(',', f);
        fprintf(f, "%d", m);
    }
    fprintf(f, "]}],\n  \"scene\":0\n}\n");
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

static void geometry_gltf_finalize_exports_locked(void)
{
    if (g_geometry.gltf_primary.enabled &&
        !geometry_gltf_write_json(&g_geometry.gltf_primary)) {
        geometry_set_error_locked("Failed to finalize geometry.gltf");
    }
    if (g_geometry.gltf_raw.enabled &&
        !geometry_gltf_write_json(&g_geometry.gltf_raw)) {
        geometry_set_error_locked("Failed to finalize geometry_raw.gltf");
    }
}

static const char *geometry_attr_name(int slot)
{
    static const char *names[NV2A_VERTEXSHADER_ATTRIBUTES] = {
        "position", "weight", "normal", "diffuse", "specular", "fog",
        "point_size", "back_diffuse", "back_specular", "texcoord0",
        "texcoord1", "texcoord2", "texcoord3", "reserved1", "reserved2",
        "reserved3",
    };
    return (slot >= 0 && slot < NV2A_VERTEXSHADER_ATTRIBUTES) ? names[slot]
                                                                : "unknown";
}

static void geometry_json_write_texture(
    FILE *f, PGRAPHState *pg, int slot,
    const GeometryTextureExportInfo *info)
{
    fprintf(f,
            "{\"slot\":%d,\"enabled\":%s,\"offset\":%u,\"format\":%u,"
            "\"control0\":%u,\"control1\":%u,\"filter\":%u,"
            "\"image_rect\":%u,\"palette\":%u,\"border_color\":%u",
            slot, (info && info->enabled) ? "true" : "false",
            pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + slot * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + slot * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + slot * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0 + slot * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + slot * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0 + slot * 4),
            pgraph_reg_r(pg, NV_PGRAPH_TEXPALETTE0 + slot * 4),
            pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + slot * 4));
    if (info && info->enabled) {
        fprintf(f,
                ",\"content_hash\":\"%016" PRIx64 "\","
                "\"width\":%u,\"height\":%u,"
                "\"decoded_width\":%u,\"decoded_height\":%u,"
                "\"cubemap\":%s,\"linear\":%s,\"border\":%s,"
                "\"pixel_texture_mode\":%u,"
                "\"pixel_texture_mode_name\":\"%s\","
                "\"stage_active\":%s,\"obj_2d_mappable\":%s,"
                "\"gltf_2d_mappable\":%s,"
                "\"nv2a_address\":%u,\"nv2a_filter\":%u,"
                "\"address_u\":%u,\"address_v\":%u,"
                "\"gltf_wrap_s\":%d,\"gltf_wrap_t\":%d,"
                "\"gltf_min_filter\":%d,\"gltf_mag_filter\":%d,"
                "\"sampler_border_approximation\":%s,"
                "\"blend_enabled\":%s,\"alpha_test_enabled\":%s,"
                "\"alpha_ref\":%u,\"alpha_func\":%u,"
                "\"dumped\":%s,\"file\":\"%s\",\"source\":\"guest_vram\"",
                info->content_hash, info->width, info->height,
                info->decoded_width, info->decoded_height,
                info->cubemap ? "true" : "false",
                info->linear ? "true" : "false",
                info->border ? "true" : "false",
                info->pixel_texture_mode,
                geometry_pixel_texture_mode_name(info->pixel_texture_mode),
                info->stage_active ? "true" : "false",
                geometry_texture_stage_obj_mappable(info) ? "true" : "false",
                geometry_texture_stage_obj_mappable(info) ? "true" : "false",
                info->address_reg, info->filter_reg,
                info->address_u, info->address_v,
                info->gltf_wrap_s, info->gltf_wrap_t,
                info->gltf_min_filter, info->gltf_mag_filter,
                info->sampler_border_approx ? "true" : "false",
                info->blend_enabled ? "true" : "false",
                info->alpha_test_enabled ? "true" : "false",
                info->alpha_ref, info->alpha_func,
                info->dumped ? "true" : "false", info->file);
    }
    fputc('}', f);
}

static void geometry_json_write_attr(FILE *f, PGRAPHState *pg, int slot,
                                     const char *name)
{
    VertexAttribute *a = &pg->vertex_attributes[slot];
    fprintf(f,
            "\"%s\":{\"slot\":%d,\"count\":%u,\"format\":%u,"
            "\"size\":%u,\"stride\":%u,\"dma\":\"%c\","
            "\"offset\":%" PRIu64 ",\"inline_buffer_populated\":%s}",
            name, slot, a->count, a->format, a->size, a->stride,
            a->dma_select ? 'B' : 'A', (uint64_t)a->offset,
            a->inline_buffer_populated ? "true" : "false");
}

static bool geometry_capture_one_draw_locked(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    if (!g_geometry.obj || !g_geometry.jsonl || !g_geometry.csv ||
        pg->primitive_mode == PRIM_TYPE_INVALID) {
        return false;
    }

    g_autoptr(GArray) vertices =
        g_array_new(FALSE, FALSE, sizeof(GeometryVertex));
    g_autoptr(GArray) placed_vertices =
        g_array_new(FALSE, FALSE, sizeof(GeometryVertex));
    g_autoptr(GArray) segments =
        g_array_new(FALSE, FALSE, sizeof(GeometrySegment));
    GeometrySourceKind source = GEOM_SOURCE_NONE;
    if (!geometry_collect_vertices(d, vertices, segments, &source)) {
        return false;
    }

    const bool has_position =
        pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION].count != 0 ||
        source == GEOM_SOURCE_INLINE_BUFFER;
    const bool has_normal =
        pg->vertex_attributes[NV2A_VERTEX_ATTR_NORMAL].count != 0 ||
        (source == GEOM_SOURCE_INLINE_BUFFER &&
         pg->vertex_attributes[NV2A_VERTEX_ATTR_NORMAL].inline_buffer_populated);
    const bool has_uv =
        pg->vertex_attributes[NV2A_VERTEX_ATTR_TEXTURE0].count != 0 ||
        (source == GEOM_SOURCE_INLINE_BUFFER &&
         pg->vertex_attributes[NV2A_VERTEX_ATTR_TEXTURE0].inline_buffer_populated);
    const bool has_diffuse =
        pg->vertex_attributes[NV2A_VERTEX_ATTR_DIFFUSE].count != 0 ||
        (source == GEOM_SOURCE_INLINE_BUFFER &&
         pg->vertex_attributes[NV2A_VERTEX_ATTR_DIFFUSE].inline_buffer_populated);
    const bool has_weight =
        pg->vertex_attributes[NV2A_VERTEX_ATTR_WEIGHT].count != 0 ||
        (source == GEOM_SOURCE_INLINE_BUFFER &&
         pg->vertex_attributes[NV2A_VERTEX_ATTR_WEIGHT].inline_buffer_populated);

    if (!has_position) {
        return false;
    }

    /* Reconstruct the texture coordinates the rasterizer actually receives.
     * This is independent of placement export and supports both the fixed
     * function pipeline and programmable NV2A vertex shaders. */
    const bool post_vsh_texcoords_evaluated =
        geometry_evaluate_post_vsh_texcoords(pg, vertices);

    const bool placed_available =
        g_geometry.placed_obj &&
        geometry_build_placed_vertices(pg, vertices, placed_vertices);
    if (g_geometry.placed_obj) {
        if (placed_available) {
            g_geometry.placed_draws_captured++;
        } else {
            g_geometry.placed_draws_unsupported++;
        }
    }

    uint64_t draw_id = ++g_geometry.draw_serial;
    uint64_t base = g_geometry.obj_vertex_base;
    uint64_t textured_base = g_geometry.textured_obj_vertex_base;
    uint64_t placed_base = g_geometry.placed_obj_vertex_base;
    uint32_t frame_index = g_geometry.active_frame_index;

    GeometryTextureExportInfo texture_info[NV2A_MAX_TEXTURES];
    memset(texture_info, 0, sizeof(texture_info));
    if (g_geometry.dump_textures) {
        for (int slot = 0; slot < NV2A_MAX_TEXTURES; ++slot) {
            (void)geometry_dump_texture_stage_locked(
                d, slot, draw_id, frame_index, &texture_info[slot]);
        }
    } else {
        for (int slot = 0; slot < NV2A_MAX_TEXTURES; ++slot) {
            texture_info[slot].enabled = pgraph_is_texture_enabled(pg, slot);
            texture_info[slot].stage_active =
                pgraph_is_texture_stage_active(pg, slot);
            texture_info[slot].pixel_texture_mode =
                (pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG) >> (slot * 5)) & 0x1F;
        }
    }

    /* Choose only a directly projectable 2D texture stage whose post-VSH
     * coordinates can be reconstructed for every vertex. V3 always bound
     * texture stage 0 to raw TEXCOORD0, which is not how NV2A works and is why
     * the Blender material mapping became badly stretched/misaligned. */
    g_autoptr(GArray) material_uvs =
        g_array_new(FALSE, FALSE, sizeof(GeometryUV));
    g_autoptr(GArray) candidate_uvs =
        g_array_new(FALSE, FALSE, sizeof(GeometryUV));
    int material_texture_slot = -1;
    unsigned int material_texture_score = 0;
    if (g_geometry.dump_textures && post_vsh_texcoords_evaluated) {
        for (int slot = 0; slot < NV2A_MAX_TEXTURES; ++slot) {
            g_array_set_size(candidate_uvs, 0);
            if (!geometry_build_material_uvs(
                    vertices, slot, &texture_info[slot], candidate_uvs)) {
                continue;
            }
            unsigned int score = geometry_combiner_texture_score(pg, slot);
            /* A directly projectable active stage remains a valid fallback if
             * the combiner contains no direct Tn reference we can prove. */
            if (texture_info[slot].stage_active) {
                score += 1;
            }
            if (material_texture_slot < 0 || score > material_texture_score) {
                material_texture_slot = slot;
                material_texture_score = score;
                g_array_set_size(material_uvs, 0);
                g_array_append_vals(material_uvs, candidate_uvs->data,
                                    candidate_uvs->len);
            }
        }
    }
    const bool material_mapped = material_texture_slot >= 0 &&
                                 material_uvs->len == vertices->len;

    char material_name[96] = { 0 };
    if (material_mapped && g_geometry.mtl) {
        snprintf(material_name, sizeof(material_name),
                 "frame_%04u_draw_%06" PRIu64, frame_index, draw_id);
        fprintf(g_geometry.mtl,
                "newmtl %s\nKd 1.0 1.0 1.0\nd 1.0\nmap_Kd %s\n\n",
                material_name, texture_info[material_texture_slot].file);
    }

    /* geometry.obj remains a raw/native evidence file. Do not attach the MTL:
     * its vt records intentionally remain the guest TEXCOORD0 input. */
    fprintf(g_geometry.obj,
            "\n# frame_index=%u draw=%" PRIu64
            " frame_time=%d source=%s primitive=%s\n"
            "o frame_%04u_draw_%06" PRIu64 "\n",
            frame_index, draw_id, pg->frame_time, geometry_source_name(source),
            geometry_primitive_name(pg->primitive_mode), frame_index, draw_id);
    for (guint i = 0; i < vertices->len; ++i) {
        const GeometryVertex *v = &g_array_index(vertices, GeometryVertex, i);
        fprintf(g_geometry.obj, "v %.9g %.9g %.9g\n",
                v->position[0] * g_geometry.export_scale,
                v->position[1] * g_geometry.export_scale,
                v->position[2] * g_geometry.export_scale);
    }
    for (guint i = 0; i < vertices->len; ++i) {
        const GeometryVertex *v = &g_array_index(vertices, GeometryVertex, i);
        fprintf(g_geometry.obj, "vt %.9g %.9g\n", v->tex0[0], v->tex0[1]);
    }
    for (guint i = 0; i < vertices->len; ++i) {
        const GeometryVertex *v = &g_array_index(vertices, GeometryVertex, i);
        fprintf(g_geometry.obj, "vn %.9g %.9g %.9g\n",
                v->normal[0], v->normal[1], v->normal[2]);
    }

    /* geometry_textured.obj uses the DCC-facing right-handed basis plus the
     * reconstructed sampling UV of the selected direct 2D stage. This is the most complete
     * material-mapped export because programmable-VSH draws are supported too. */
    if (g_geometry.textured_obj) {
        fprintf(g_geometry.textured_obj,
                "\n# frame_index=%u draw=%" PRIu64
                " frame_time=%d source=%s primitive=%s material_stage=%d\n"
                "o frame_%04u_draw_%06" PRIu64 "\n",
                frame_index, draw_id, pg->frame_time,
                geometry_source_name(source),
                geometry_primitive_name(pg->primitive_mode),
                material_texture_slot, frame_index, draw_id);
        if (material_name[0]) {
            fprintf(g_geometry.textured_obj, "usemtl %s\n", material_name);
        }
        for (guint i = 0; i < vertices->len; ++i) {
            const GeometryVertex *v =
                &g_array_index(vertices, GeometryVertex, i);
            fprintf(g_geometry.textured_obj, "v %.9g %.9g %.9g\n",
                    v->position[0] * g_geometry.export_scale,
                    v->position[1] * g_geometry.export_scale,
                    -v->position[2] * g_geometry.export_scale);
        }
        for (guint i = 0; i < vertices->len; ++i) {
            if (material_mapped) {
                const GeometryUV *uv =
                    &g_array_index(material_uvs, GeometryUV, i);
                fprintf(g_geometry.textured_obj, "vt %.9g %.9g\n",
                        uv->u, 1.0f - uv->v);
            } else {
                /* Keep OBJ v/vt/vn indices parallel even for an unmapped draw;
                 * faces simply omit vt references in that case. */
                fprintf(g_geometry.textured_obj, "vt 0 0\n");
            }
        }
        for (guint i = 0; i < vertices->len; ++i) {
            const GeometryVertex *v =
                &g_array_index(vertices, GeometryVertex, i);
            fprintf(g_geometry.textured_obj, "vn %.9g %.9g %.9g\n",
                    v->normal[0], v->normal[1], -v->normal[2]);
        }
    }

    if (placed_available) {
        fprintf(g_geometry.placed_obj,
                "\n# frame_index=%u draw=%" PRIu64
                " frame_time=%d source=%s primitive=%s material_stage=%d\n"
                "o frame_%04u_draw_%06" PRIu64 "\n",
                frame_index, draw_id, pg->frame_time,
                geometry_source_name(source),
                geometry_primitive_name(pg->primitive_mode),
                material_texture_slot, frame_index, draw_id);
        if (material_name[0]) {
            fprintf(g_geometry.placed_obj, "usemtl %s\n", material_name);
        }
        for (guint i = 0; i < placed_vertices->len; ++i) {
            const GeometryVertex *v =
                &g_array_index(placed_vertices, GeometryVertex, i);
            fprintf(g_geometry.placed_obj, "v %.9g %.9g %.9g\n",
                    v->position[0] * g_geometry.export_scale,
                    v->position[1] * g_geometry.export_scale,
                    -v->position[2] * g_geometry.export_scale);
        }
        for (guint i = 0; i < placed_vertices->len; ++i) {
            if (material_mapped) {
                const GeometryUV *uv =
                    &g_array_index(material_uvs, GeometryUV, i);
                fprintf(g_geometry.placed_obj, "vt %.9g %.9g\n",
                        uv->u, 1.0f - uv->v);
            } else {
                fprintf(g_geometry.placed_obj, "vt 0 0\n");
            }
        }
        for (guint i = 0; i < placed_vertices->len; ++i) {
            const GeometryVertex *v =
                &g_array_index(placed_vertices, GeometryVertex, i);
            fprintf(g_geometry.placed_obj, "vn %.9g %.9g %.9g\n",
                    v->normal[0], v->normal[1], -v->normal[2]);
        }
    }

    /* glTF is the primary scene/model interchange export. geometry.gltf uses
     * placed fixed-function vertices when placement export is enabled; otherwise
     * it uses raw/local positions. When placed export is enabled, a complete
     * geometry_raw.gltf companion keeps every raw draw, including programmable
     * vertex-shader draws that cannot be generically placed. */
    for (guint si = 0; si < segments->len; ++si) {
        const GeometrySegment *seg =
            &g_array_index(segments, GeometrySegment, si);
        const GeometryTextureExportInfo *mat_info =
            material_mapped ? &texture_info[material_texture_slot] : NULL;
        if (g_geometry.gltf_primary.enabled) {
            if (g_geometry.gltf_primary.placed) {
                if (placed_available) {
                    (void)geometry_gltf_append_primitive(
                        &g_geometry.gltf_primary, placed_vertices, seg, si,
                        pg->primitive_mode, source, frame_index, draw_id,
                        has_normal, material_mapped, material_uvs,
                        material_texture_slot, mat_info);
                }
            } else {
                (void)geometry_gltf_append_primitive(
                    &g_geometry.gltf_primary, vertices, seg, si,
                    pg->primitive_mode, source, frame_index, draw_id,
                    has_normal, material_mapped, material_uvs,
                    material_texture_slot, mat_info);
            }
        }
        if (g_geometry.gltf_raw.enabled) {
            (void)geometry_gltf_append_primitive(
                &g_geometry.gltf_raw, vertices, seg, si,
                pg->primitive_mode, source, frame_index, draw_id,
                has_normal, material_mapped, material_uvs,
                material_texture_slot, mat_info);
        }
    }

    for (guint si = 0; si < segments->len; ++si) {
        const GeometrySegment *seg =
            &g_array_index(segments, GeometrySegment, si);
        for (uint32_t j = 0; j < seg->vertex_count; ++j) {
            uint32_t vi = seg->first_vertex + j;
            const GeometryVertex *v =
                &g_array_index(vertices, GeometryVertex, vi);
            const GeometryVertex *pv = placed_available
                ? &g_array_index(placed_vertices, GeometryVertex, vi) : NULL;
            fprintf(g_geometry.csv,
                    "%u,%d,%" PRIu64 ",%u,%u,%u,"
                    "%.9g,%.9g,%.9g,%.9g,"
                    "%.9g,%.9g,%.9g,%.9g,"
                    "%.9g,%.9g,%.9g,%.9g,"
                    "%.9g,%.9g,%.9g,%.9g,"
                    "%.9g,%.9g,%.9g,%.9g,"
                    "%s,%.9g,%.9g,%.9g,%.9g\n",
                    frame_index, pg->frame_time, draw_id, si, j, v->source_index,
                    v->position[0], v->position[1], v->position[2], v->position[3],
                    v->weight[0], v->weight[1], v->weight[2], v->weight[3],
                    v->normal[0], v->normal[1], v->normal[2], v->normal[3],
                    v->tex0[0], v->tex0[1], v->tex0[2], v->tex0[3],
                    v->diffuse[0], v->diffuse[1], v->diffuse[2], v->diffuse[3],
                    pv ? "true" : "false",
                    pv ? pv->position[0] : 0.0f,
                    pv ? pv->position[1] : 0.0f,
                    pv ? pv->position[2] : 0.0f,
                    pv ? pv->position[3] : 0.0f);
        }
    }

    uint64_t primitive_count = 0;
    for (guint si = 0; si < segments->len; ++si) {
        const GeometrySegment *seg =
            &g_array_index(segments, GeometrySegment, si);
        fprintf(g_geometry.obj,
                "g frame_%04u_draw_%06" PRIu64 "_segment_%u\n",
                frame_index, draw_id, si);
        primitive_count += geometry_write_segment(
            g_geometry.obj, pg->primitive_mode,
            base + seg->first_vertex,
            &g_array_index(vertices, GeometryVertex, 0),
            seg->first_vertex, seg->vertex_count, has_uv, has_normal, false);

        if (g_geometry.textured_obj) {
            fprintf(g_geometry.textured_obj,
                    "g frame_%04u_draw_%06" PRIu64 "_segment_%u\n",
                    frame_index, draw_id, si);
            (void)geometry_write_segment(
                g_geometry.textured_obj, pg->primitive_mode,
                textured_base + seg->first_vertex,
                &g_array_index(vertices, GeometryVertex, 0),
                seg->first_vertex, seg->vertex_count,
                material_mapped, has_normal, true);
        }

        if (placed_available) {
            fprintf(g_geometry.placed_obj,
                    "g frame_%04u_draw_%06" PRIu64 "_segment_%u\n",
                    frame_index, draw_id, si);
            (void)geometry_write_segment(
                g_geometry.placed_obj, pg->primitive_mode,
                placed_base + seg->first_vertex,
                &g_array_index(placed_vertices, GeometryVertex, 0),
                seg->first_vertex, seg->vertex_count,
                material_mapped, has_normal, true);
        }
    }

    uint32_t setup_raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    unsigned int transform_mode = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_D), NV_PGRAPH_CSV0_D_MODE);
    unsigned int skinning = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_D), NV_PGRAPH_CSV0_D_SKIN);
    unsigned int program_start = GET_MASK(
        pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
        NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);

    fprintf(g_geometry.jsonl,
            "{\"type\":\"draw\",\"draw\":%" PRIu64
            ",\"frame_index\":%u,\"frame_time\":%d,\"primitive\":\"%s\","
            "\"primitive_mode\":%u,\"source\":\"%s\","
            "\"vertices\":%u,\"segments\":%u,\"primitives\":%" PRIu64
            ",\"has_position\":%s,\"has_weight\":%s,\"has_normal\":%s,"
            "\"has_texcoord0\":%s,\"has_diffuse\":%s,"
            "\"placed_position_available\":%s,"
            "\"post_vsh_texcoords_evaluated\":%s,"
            "\"obj_material_mapped\":%s,\"gltf_material_mapped\":%s,"
            "\"obj_material_texture_slot\":%d,"
            "\"obj_material_texture_score\":%u,"
            "\"obj_uv_source\":\"%s\","
            "\"transform_mode\":\"%s\",\"transform_mode_raw\":%u,"
            "\"skinning_mode\":%u,\"program_start\":%u,"
            "\"guest_cull_enabled\":%s,\"guest_cull_face\":%u,"
            "\"guest_front_face\":\"%s\","
            "\"capture_cull_override\":%s,"
            "\"dma_vertex_a\":%" PRIu64 ",\"dma_vertex_b\":%" PRIu64
            ",\"segment_vertex_counts\":[",
            draw_id, frame_index, pg->frame_time,
            geometry_primitive_name(pg->primitive_mode), pg->primitive_mode,
            geometry_source_name(source), vertices->len, segments->len,
            primitive_count, has_position ? "true" : "false",
            has_weight ? "true" : "false", has_normal ? "true" : "false",
            has_uv ? "true" : "false", has_diffuse ? "true" : "false",
            placed_available ? "true" : "false",
            post_vsh_texcoords_evaluated ? "true" : "false",
            material_mapped ? "true" : "false",
            material_mapped ? "true" : "false", material_texture_slot,
            material_texture_score,
            material_mapped ? "post_vertex_shader_project2d" : "none",
            transform_mode == 0 ? "fixed_function" : "programmable",
            transform_mode, skinning, program_start,
            (setup_raster & NV_PGRAPH_SETUPRASTER_CULLENABLE) ? "true" : "false",
            GET_MASK(setup_raster, NV_PGRAPH_SETUPRASTER_CULLCTRL),
            (setup_raster & NV_PGRAPH_SETUPRASTER_FRONTFACE) ? "ccw" : "cw",
            g_geometry.disable_backface_culling ? "true" : "false",
            (uint64_t)pg->dma_vertex_a, (uint64_t)pg->dma_vertex_b);

    for (guint si = 0; si < segments->len; ++si) {
        const GeometrySegment *seg =
            &g_array_index(segments, GeometrySegment, si);
        if (si) {
            fputc(',', g_geometry.jsonl);
        }
        fprintf(g_geometry.jsonl, "%u", seg->vertex_count);
    }

    fprintf(g_geometry.jsonl, "],\"attributes\":{");
    for (int slot = 0; slot < NV2A_VERTEXSHADER_ATTRIBUTES; ++slot) {
        if (slot) {
            fputc(',', g_geometry.jsonl);
        }
        geometry_json_write_attr(g_geometry.jsonl, pg, slot,
                                 geometry_attr_name(slot));
    }

    fprintf(g_geometry.jsonl, "},\"textures\":[");
    for (int slot = 0; slot < NV2A_MAX_TEXTURES; ++slot) {
        if (slot) {
            fputc(',', g_geometry.jsonl);
        }
        geometry_json_write_texture(g_geometry.jsonl, pg, slot,
                                    &texture_info[slot]);
    }

    fprintf(g_geometry.jsonl, "],\"transform_constants\":{");
    fprintf(g_geometry.jsonl, "\"composite\":");
    geometry_json_write_const_matrix(g_geometry.jsonl, pg,
                                     NV_IGRAPH_XF_XFCTX_CMAT0);
    fprintf(g_geometry.jsonl, ",\"projection\":");
    geometry_json_write_const_matrix(g_geometry.jsonl, pg,
                                     NV_IGRAPH_XF_XFCTX_PMAT0);
    fprintf(g_geometry.jsonl, ",\"modelview\":[");
    for (unsigned int i = 0; i < 4; ++i) {
        if (i) {
            fputc(',', g_geometry.jsonl);
        }
        geometry_json_write_const_matrix(g_geometry.jsonl, pg,
                                         NV_IGRAPH_XF_XFCTX_MMAT0 + i * 8);
    }
    fprintf(g_geometry.jsonl, "],\"texture_matrices\":[");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; ++i) {
        if (i) {
            fputc(',', g_geometry.jsonl);
        }
        geometry_json_write_const_matrix(g_geometry.jsonl, pg,
                                         NV_IGRAPH_XF_XFCTX_T0MAT + i * 8);
    }
    fprintf(g_geometry.jsonl, "],\"texture_matrix_enable\":[");
    for (unsigned int i = 0; i < NV2A_MAX_TEXTURES; ++i) {
        if (i) {
            fputc(',', g_geometry.jsonl);
        }
        fprintf(g_geometry.jsonl, "%s",
                pg->texture_matrix_enable[i] ? "true" : "false");
    }
    fprintf(g_geometry.jsonl, "]}}\n");

    g_geometry.obj_vertex_base += vertices->len;
    if (g_geometry.textured_obj) {
        g_geometry.textured_obj_vertex_base += vertices->len;
    }
    if (placed_available) {
        g_geometry.placed_obj_vertex_base += placed_vertices->len;
    }
    g_geometry.draws_captured++;
    g_geometry.vertices_captured += vertices->len;
    g_geometry.primitives_captured += primitive_count;
    return true;
}

static bool geometry_draw_has_geometry(PGRAPHState *pg)
{
    if (!pg || pg->primitive_mode == PRIM_TYPE_INVALID) {
        return false;
    }
    const bool has_vertices =
        pg->draw_arrays_length || pg->inline_elements_length ||
        pg->inline_buffer_length || pg->inline_array_length;
    if (!has_vertices) {
        return false;
    }

    VertexAttribute *position =
        &pg->vertex_attributes[NV2A_VERTEX_ATTR_POSITION];
    if (pg->inline_buffer_length) {
        return position->inline_buffer_populated;
    }
    return position->count != 0;
}

static bool geometry_capture_before_backend_draw(NV2AState *d)
{
    bool disable_cull_for_render = false;
    int mode = qatomic_read(&g_geometry.mode);

    if (mode == XEMU_GEOMETRY_CAPTURE_IDLE) {
        return false;
    }

    qemu_mutex_lock(&g_geometry.lock);
    mode = qatomic_read(&g_geometry.mode);
    const bool has_geometry = geometry_draw_has_geometry(&d->pgraph);

    if (mode == XEMU_GEOMETRY_CAPTURE_NEXT_DRAW && has_geometry) {
        disable_cull_for_render = g_geometry.disable_backface_culling;
        if (geometry_begin_capture_locked(d->pgraph.frame_time)) {
            geometry_begin_frame_locked(d->pgraph.frame_time);
            if (geometry_capture_one_draw_locked(d)) {
                (void)geometry_complete_frame_locked();
            } else {
                geometry_close_files_locked();
                g_geometry.output_path[0] = '\0';
                g_geometry.current_frame_started = false;
            }
        } else {
            geometry_close_files_locked();
            qatomic_set(&g_geometry.mode, XEMU_GEOMETRY_CAPTURE_IDLE);
        }
    } else if (mode == XEMU_GEOMETRY_CAPTURE_NEXT_FRAME_WAIT &&
               !qatomic_read(&g_geometry.flip_stall_seen)) {
        /* frame_time fallback when a title does not issue FLIP_STALL. */
        if (g_geometry.wait_frame_time == INT32_MIN) {
            g_geometry.wait_frame_time = d->pgraph.frame_time;
        } else if (d->pgraph.frame_time != g_geometry.wait_frame_time &&
                   has_geometry) {
            if (geometry_begin_capture_locked(d->pgraph.frame_time)) {
                qatomic_set(&g_geometry.mode,
                            XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE);
                geometry_begin_frame_locked(d->pgraph.frame_time);
                geometry_capture_one_draw_locked(d);
                disable_cull_for_render =
                    g_geometry.disable_backface_culling;
            } else {
                geometry_close_files_locked();
                qatomic_set(&g_geometry.mode, XEMU_GEOMETRY_CAPTURE_IDLE);
            }
        }
    } else if (mode == XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE) {
        if (!g_geometry.current_frame_started) {
            geometry_begin_frame_locked(d->pgraph.frame_time);
        } else if (d->pgraph.frame_time != g_geometry.frame_time) {
            bool finished = geometry_complete_frame_locked();
            if (!finished) {
                geometry_begin_frame_locked(d->pgraph.frame_time);
            }
        }

        if (qatomic_read(&g_geometry.mode) ==
                XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE && has_geometry) {
            geometry_capture_one_draw_locked(d);
            disable_cull_for_render =
                g_geometry.disable_backface_culling;
        }
    }
    qemu_mutex_unlock(&g_geometry.lock);
    return disable_cull_for_render;
}

static void geometry_call_backend_draw(NV2AState *d,
                                       void (*callback)(NV2AState *d),
                                       bool disable_cull_for_render)
{
    if (!callback) {
        return;
    }

    /* Normal gameplay never asks the capture layer to override culling.
     * Keep that overwhelmingly common path identical to the backend call and
     * avoid an NV2A register read on every draw just to discover that no
     * state change is required. */
    if (!disable_cull_for_render) {
        callback(d);
        return;
    }

    PGRAPHState *pg = &d->pgraph;
    uint32_t saved_setup_raster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    const bool cull_was_enabled =
        saved_setup_raster & NV_PGRAPH_SETUPRASTER_CULLENABLE;

    if (cull_was_enabled) {
        pgraph_reg_w(pg, NV_PGRAPH_SETUPRASTER,
                     saved_setup_raster & ~NV_PGRAPH_SETUPRASTER_CULLENABLE);
    }

    callback(d);

    if (cull_was_enabled) {
        pgraph_reg_w(pg, NV_PGRAPH_SETUPRASTER, saved_setup_raster);
    }
}

/* Freecam needs to edit the fixed-function transform before the backend's
 * draw_begin uploads shader state. Keep the transform active through all
 * partial flushes and restore it only after the matching draw_end. */
static void geometry_capture_draw_begin(NV2AState *d)
{
    xemu_freecam_renderer_draw_begin(d);
    if (g_geometry_original_draw_begin) {
        g_geometry_original_draw_begin(d);
    }
}

/* Normal NV097 BEGIN/END submission reaches renderer->ops.draw_end().  GL and
 * Vulkan draw_end then call their backend flush helpers directly, so observing
 * only ops.flush_draw misses virtually every ordinary Xbox draw.  Wrap both:
 * draw_end covers normal batches; flush_draw covers the uncommon partial batch
 * emitted by pgraph_expand_draw_arrays() before END. */
static void geometry_capture_draw_end(NV2AState *d)
{
    geometry_material_update_camera_headlight(d);
    const bool disable_cull = geometry_capture_before_backend_draw(d);
    geometry_call_backend_draw(d, g_geometry_original_draw_end, disable_cull);
    xemu_freecam_renderer_draw_end(d);
}

static void geometry_capture_flush_draw(NV2AState *d)
{
    geometry_material_update_camera_headlight(d);
    const bool disable_cull = geometry_capture_before_backend_draw(d);
    geometry_call_backend_draw(d, g_geometry_original_flush_draw, disable_cull);
}

static void geometry_capture_flip_stall(NV2AState *d)
{
    xemu_freecam_renderer_abort_draw(d);
    int mode = qatomic_read(&g_geometry.mode);

    /* flip_stall_seen is capture bookkeeping, not general renderer state.
     * Avoid a cache-line write every displayed frame while the dumper is
     * idle. */
    if (mode != XEMU_GEOMETRY_CAPTURE_IDLE) {
        qatomic_set(&g_geometry.flip_stall_seen, 1);
    }

    /* FLIP_STALL closes the frame that was just rendered. */
    if (mode == XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE) {
        qemu_mutex_lock(&g_geometry.lock);
        if (qatomic_read(&g_geometry.mode) ==
            XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE) {
            if (!g_geometry.current_frame_started) {
                geometry_begin_frame_locked(d->pgraph.frame_time);
            }
            (void)geometry_complete_frame_locked();
        }
        qemu_mutex_unlock(&g_geometry.lock);
    }

    if (g_geometry_original_flip_stall) {
        g_geometry_original_flip_stall(d);
    }

    /* Start after the next completed flip so frame zero is complete. */
    if (qatomic_read(&g_geometry.mode) ==
        XEMU_GEOMETRY_CAPTURE_NEXT_FRAME_WAIT) {
        qemu_mutex_lock(&g_geometry.lock);
        if (qatomic_read(&g_geometry.mode) ==
            XEMU_GEOMETRY_CAPTURE_NEXT_FRAME_WAIT) {
            if (geometry_begin_capture_locked(d->pgraph.frame_time)) {
                g_geometry.frame_time = INT32_MIN;
                g_geometry.current_frame_started = false;
                qatomic_set(&g_geometry.mode,
                            XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE);
            } else {
                geometry_close_files_locked();
                qatomic_set(&g_geometry.mode, XEMU_GEOMETRY_CAPTURE_IDLE);
            }
        }
        qemu_mutex_unlock(&g_geometry.lock);
    }
}

void xemu_geometry_dumper_renderer_ready(void)
{
    geometry_init_once();
    NV2AState *d = g_nv2a;
    if (!d || !d->pgraph.renderer) {
        return;
    }

    qemu_mutex_lock(&g_geometry.lock);
    if (d->pgraph.renderer == &g_geometry_renderer) {
        g_geometry.renderer_hooked = true;
        qemu_mutex_unlock(&g_geometry.lock);
        return;
    }

    /* Copy, do not modify, the upstream renderer descriptor. */
    g_geometry_renderer = *d->pgraph.renderer;
    g_geometry_original_draw_begin = d->pgraph.renderer->ops.draw_begin;
    g_geometry_original_draw_end = d->pgraph.renderer->ops.draw_end;
    g_geometry_original_flush_draw = d->pgraph.renderer->ops.flush_draw;
    g_geometry_original_flip_stall = d->pgraph.renderer->ops.flip_stall;
    g_geometry_renderer.ops.draw_begin = geometry_capture_draw_begin;
    g_geometry_renderer.ops.draw_end = geometry_capture_draw_end;
    g_geometry_renderer.ops.flush_draw = geometry_capture_flush_draw;
    g_geometry_renderer.ops.flip_stall = geometry_capture_flip_stall;
    d->pgraph.renderer = &g_geometry_renderer;
    g_geometry.renderer_hooked = true;
    qemu_mutex_unlock(&g_geometry.lock);
}

static XemuGeometryCaptureOptions geometry_default_options(void)
{
    XemuGeometryCaptureOptions options = {
        .frame_count = 1,
        .disable_backface_culling = false,
        .export_placed_geometry = false,
        .export_scale = 1.0f,
        .dump_textures = false,
    };
    return options;
}

static bool geometry_request_capture(XemuGeometryCaptureMode mode,
                                     const char *output_root,
                                     const XemuGeometryCaptureOptions *requested)
{
    geometry_init_once();

    XemuGeometryCaptureOptions options = requested
        ? *requested : geometry_default_options();
    options.frame_count = MAX(1u, MIN(options.frame_count, 10000u));
    if (!isfinite(options.export_scale) || options.export_scale <= 0.0f) {
        options.export_scale = 1.0f;
    }
    options.export_scale = MIN(options.export_scale, 1000.0f);
    if (mode == XEMU_GEOMETRY_CAPTURE_NEXT_DRAW) {
        options.frame_count = 1;
    }

    uint32_t title_id = 0;
    (void)xemu_get_xbe_title_id(&title_id);

    g_autofree char *prepared_root = NULL;
    if (output_root && output_root[0]) {
        prepared_root = g_strdup(output_root);
    } else {
        prepared_root = g_build_filename(xemu_settings_get_base_path(),
                                         "geometry", NULL);
    }

    qemu_mutex_lock(&g_geometry.lock);
    if (!g_geometry.renderer_hooked) {
        geometry_set_error_locked(
            "NV2A renderer is not ready yet. Start a game, then capture again.");
        qemu_mutex_unlock(&g_geometry.lock);
        return false;
    }
    if (qatomic_read(&g_geometry.mode) != XEMU_GEOMETRY_CAPTURE_IDLE) {
        geometry_set_error_locked("A geometry capture is already pending/active.");
        qemu_mutex_unlock(&g_geometry.lock);
        return false;
    }

    g_strlcpy(g_geometry.request_root, prepared_root ? prepared_root : "",
              sizeof(g_geometry.request_root));
    g_geometry.request_title_id = title_id;
    g_geometry.frames_requested = options.frame_count;
    g_geometry.frames_completed = 0;
    g_geometry.active_frame_index = 0;
    g_geometry.disable_backface_culling = options.disable_backface_culling;
    g_geometry.export_placed_geometry = options.export_placed_geometry;
    g_geometry.export_scale = options.export_scale;
    g_geometry.dump_textures = options.dump_textures;
    g_geometry.current_frame_started = false;
    g_geometry.wait_frame_time = INT32_MIN;
    g_geometry.frame_time = INT32_MIN;
    g_geometry.last_error[0] = '\0';
    qatomic_set(&g_geometry.flip_stall_seen, 0);
    qatomic_set(&g_geometry.mode, mode);
    qemu_mutex_unlock(&g_geometry.lock);
    return true;
}

bool xemu_geometry_dumper_capture_next_draw(const char *output_root)
{
    XemuGeometryCaptureOptions options = geometry_default_options();
    return geometry_request_capture(XEMU_GEOMETRY_CAPTURE_NEXT_DRAW,
                                    output_root, &options);
}

bool xemu_geometry_dumper_capture_next_frame(const char *output_root)
{
    XemuGeometryCaptureOptions options = geometry_default_options();
    return geometry_request_capture(XEMU_GEOMETRY_CAPTURE_NEXT_FRAME_WAIT,
                                    output_root, &options);
}

bool xemu_geometry_dumper_capture_next_draw_ex(
    const char *output_root, const XemuGeometryCaptureOptions *options)
{
    return geometry_request_capture(XEMU_GEOMETRY_CAPTURE_NEXT_DRAW,
                                    output_root, options);
}

bool xemu_geometry_dumper_capture_frames(
    const char *output_root, const XemuGeometryCaptureOptions *options)
{
    return geometry_request_capture(XEMU_GEOMETRY_CAPTURE_NEXT_FRAME_WAIT,
                                    output_root, options);
}

void xemu_geometry_dumper_cancel_capture(void)
{
    geometry_init_once();
    qemu_mutex_lock(&g_geometry.lock);
    if (qatomic_read(&g_geometry.mode) != XEMU_GEOMETRY_CAPTURE_IDLE) {
        if (g_geometry.jsonl) {
            fprintf(g_geometry.jsonl,
                    "{\"type\":\"capture_cancelled\",\"frames_completed\":%u,"
                    "\"frames_requested\":%u}\n",
                    g_geometry.frames_completed, g_geometry.frames_requested);
        }
        if (g_geometry.obj) {
            fprintf(g_geometry.obj,
                    "\n# capture cancelled after %u/%u complete frame(s)\n",
                    g_geometry.frames_completed, g_geometry.frames_requested);
        }
        if (g_geometry.placed_obj) {
            fprintf(g_geometry.placed_obj,
                    "\n# capture cancelled after %u/%u complete frame(s)\n",
                    g_geometry.frames_completed, g_geometry.frames_requested);
        }
        geometry_gltf_finalize_exports_locked();
        geometry_close_files_locked();
        g_geometry.current_frame_started = false;
        qatomic_set(&g_geometry.mode, XEMU_GEOMETRY_CAPTURE_IDLE);
    }
    qemu_mutex_unlock(&g_geometry.lock);
}

void xemu_geometry_dumper_get_status(XemuGeometryDumperStatus *status)
{
    if (!status) {
        return;
    }
    geometry_init_once();
    qemu_mutex_lock(&g_geometry.lock);
    memset(status, 0, sizeof(*status));
    status->renderer_hooked = g_geometry.renderer_hooked;
    status->mode = (XemuGeometryCaptureMode)qatomic_read(&g_geometry.mode);
    status->capture_serial = g_geometry.capture_serial;
    status->draws_captured = g_geometry.draws_captured;
    status->vertices_captured = g_geometry.vertices_captured;
    status->primitives_captured = g_geometry.primitives_captured;
    status->placed_draws_captured = g_geometry.placed_draws_captured;
    status->placed_draws_unsupported = g_geometry.placed_draws_unsupported;
    status->frames_requested = g_geometry.frames_requested;
    status->frames_completed = g_geometry.frames_completed;
    status->active_frame_index = g_geometry.active_frame_index;
    status->disable_backface_culling = g_geometry.disable_backface_culling;
    status->export_placed_geometry = g_geometry.export_placed_geometry;
    status->export_scale = g_geometry.export_scale;
    status->dump_textures = g_geometry.dump_textures;
    status->textures_referenced = g_geometry.textures_referenced;
    status->textures_dumped = g_geometry.textures_dumped;
    status->texture_dump_failures = g_geometry.texture_dump_failures;
    status->frame_time = g_geometry.frame_time;
    g_strlcpy(status->output_path, g_geometry.output_path,
              sizeof(status->output_path));
    g_strlcpy(status->last_error, g_geometry.last_error,
              sizeof(status->last_error));
    qemu_mutex_unlock(&g_geometry.lock);
}

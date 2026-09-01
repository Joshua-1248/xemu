/* SPDX-License-Identifier: GPL-2.0-or-later */
/* xemu custom fork - texture dump/replacement public boundary */
#ifndef XEMU_FEATURES_TEXTURE_PACKS_H
#define XEMU_FEATURES_TEXTURE_PACKS_H
#include "config-host.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XEMU_TEXTURE_PACKS_BACKEND_NONE = 0,
    XEMU_TEXTURE_PACKS_BACKEND_GL,
    XEMU_TEXTURE_PACKS_BACKEND_VK,
} XemuTexturePacksBackend;


typedef enum {
    XEMU_TEXTURE_PACKS_MATERIAL_LIGHT_HEADLIGHT = 0,
    XEMU_TEXTURE_PACKS_MATERIAL_LIGHT_DIRECTIONAL = 1,
} XemuTexturePacksMaterialLightMode;

typedef struct XemuTexturePacksMaterialConfig {
    bool enabled;
    bool flip_normal_y;
    float normal_strength;
    float ambient_strength;
    float diffuse_strength;
    float specular_strength;
    float specular_power;
    float parallax_scale;
    float ao_strength;
    int light_mode;
    float light_dir[3];
} XemuTexturePacksMaterialConfig;

typedef struct XemuTexturePacksFileStamp {
    uint64_t write_time;
    uint64_t size;
} XemuTexturePacksFileStamp;

static inline bool xemu_texture_packs_file_stamp_equal(
    const XemuTexturePacksFileStamp *a, const XemuTexturePacksFileStamp *b)
{
    return a->write_time == b->write_time && a->size == b->size;
}

#ifdef CONFIG_XEMU_FEATURE_TEXTURE_PACKS
bool xemu_texture_packs_dump_enabled(void);
bool xemu_texture_packs_replace_enabled(void);
bool xemu_texture_packs_dump_mipmaps(void);
bool xemu_texture_packs_should_dump_level(unsigned int level);
bool xemu_texture_packs_dynamic_enabled(void);
void xemu_texture_packs_refresh_paths(void);
bool xemu_texture_packs_ready(void);
bool xemu_texture_packs_has_dynamic_replacements(void);
void xemu_texture_packs_dump(uint64_t hash, unsigned int width, unsigned int height,
                             const uint8_t *rgba_data);
void xemu_texture_packs_rebuild_replacement_index(void);
void xemu_texture_packs_rebuild_dump_index(void);
bool xemu_texture_packs_get_replacement_size(uint64_t hash, int *width, int *height);
uint8_t *xemu_texture_packs_load_replacement_rgba(uint64_t hash, int *width, int *height);
void xemu_texture_packs_free_pixels(uint8_t *pixels);
void xemu_texture_packs_dump_variant(uint64_t hash, const char *variant,
                                     unsigned int width, unsigned int height,
                                     const uint8_t *rgba_data);
void xemu_texture_packs_dump_guest32_variant(uint64_t hash, const char *variant,
                                             unsigned int width, unsigned int height,
                                             unsigned int row_stride,
                                             unsigned int color_format,
                                             const uint8_t *pixel_data);
bool xemu_texture_packs_get_replacement_size_variant(uint64_t hash, const char *variant,
                                                      int *width, int *height);
uint8_t *xemu_texture_packs_load_replacement_rgba_variant(uint64_t hash, const char *variant,
                                                          int *width, int *height);
bool xemu_texture_packs_has_all_cubemap_faces(uint64_t hash, int *width, int *height);
const char *xemu_texture_packs_cubemap_face_name(int face);
bool xemu_texture_packs_replacement_is_animated(uint64_t hash, const char *variant);
const char *xemu_texture_packs_get_shader_path(uint64_t hash, const char *variant);
bool xemu_texture_packs_get_file_stamp(const char *path,
                                       XemuTexturePacksFileStamp *stamp);
int64_t xemu_texture_packs_anim_now_us(void);
int xemu_texture_packs_animated_frame_index(uint64_t hash, const char *variant, int64_t now_us);
const uint8_t *xemu_texture_packs_animated_frame_pixels(uint64_t hash, const char *variant,
                                                        int frame, int *width, int *height);
void xemu_texture_packs_set_backend(XemuTexturePacksBackend backend);
XemuTexturePacksBackend xemu_texture_packs_get_backend(void);
void xemu_texture_packs_request_cache_flush(void);
bool xemu_texture_packs_consume_flush_request(void);
void xemu_texture_packs_renderer_sync(void (*flush_backend)(void));
void xemu_texture_packs_get_material_config(XemuTexturePacksMaterialConfig *out_config);
void xemu_texture_packs_set_material_config(const XemuTexturePacksMaterialConfig *config);
bool xemu_texture_packs_material_enhancement_enabled(void);
bool xemu_texture_packs_material_sidecars_present(uint64_t hash);
uint64_t xemu_texture_packs_material_config_revision(void);
bool xemu_texture_packs_material_camera_tracking_needed(void);
bool xemu_texture_packs_material_bound_hash(void *pgraph, int stage,
                                             uint64_t *out_hash);
void xemu_texture_packs_material_set_hash_view_light(uint64_t hash,
                                                      const float dir[3]);
uint64_t xemu_texture_packs_material_light_revision(void);
typedef void (*XemuTexturePacksMaterialDrawRefreshFn)(void *opaque, int stage,
                                                       uint64_t hash);
void xemu_texture_packs_material_set_draw_refresh_callback(
    XemuTexturePacksMaterialDrawRefreshFn callback);
void xemu_texture_packs_material_refresh_draw(void *opaque, int stage,
                                              uint64_t hash);
void xemu_texture_packs_material_get_hash_view_light(uint64_t hash,
                                                      float dir[3],
                                                      uint64_t *revision);
#else
static inline bool xemu_texture_packs_dump_enabled(void) { return false; }
static inline bool xemu_texture_packs_replace_enabled(void) { return false; }
static inline bool xemu_texture_packs_dump_mipmaps(void) { return false; }
static inline bool xemu_texture_packs_should_dump_level(unsigned int level) { (void)level; return false; }
static inline bool xemu_texture_packs_dynamic_enabled(void) { return false; }
static inline void xemu_texture_packs_refresh_paths(void) {}
static inline bool xemu_texture_packs_ready(void) { return false; }
static inline bool xemu_texture_packs_has_dynamic_replacements(void) { return false; }
static inline void xemu_texture_packs_dump(uint64_t hash, unsigned int width, unsigned int height,
                                           const uint8_t *rgba_data)
{ (void)hash; (void)width; (void)height; (void)rgba_data; }
static inline void xemu_texture_packs_rebuild_replacement_index(void) {}
static inline void xemu_texture_packs_rebuild_dump_index(void) {}
static inline bool xemu_texture_packs_get_replacement_size(uint64_t hash, int *width, int *height)
{ (void)hash; (void)width; (void)height; return false; }
static inline uint8_t *xemu_texture_packs_load_replacement_rgba(uint64_t hash, int *width, int *height)
{ (void)hash; (void)width; (void)height; return NULL; }
static inline void xemu_texture_packs_free_pixels(uint8_t *pixels) { (void)pixels; }
static inline void xemu_texture_packs_dump_variant(uint64_t hash, const char *variant,
                                                   unsigned int width, unsigned int height,
                                                   const uint8_t *rgba_data)
{ (void)hash; (void)variant; (void)width; (void)height; (void)rgba_data; }
static inline void xemu_texture_packs_dump_guest32_variant(uint64_t hash, const char *variant,
                                                           unsigned int width, unsigned int height,
                                                           unsigned int row_stride,
                                                           unsigned int color_format,
                                                           const uint8_t *pixel_data)
{ (void)hash; (void)variant; (void)width; (void)height; (void)row_stride; (void)color_format; (void)pixel_data; }
static inline bool xemu_texture_packs_get_replacement_size_variant(uint64_t hash, const char *variant,
                                                                    int *width, int *height)
{ (void)hash; (void)variant; (void)width; (void)height; return false; }
static inline uint8_t *xemu_texture_packs_load_replacement_rgba_variant(uint64_t hash, const char *variant,
                                                                         int *width, int *height)
{ (void)hash; (void)variant; (void)width; (void)height; return NULL; }
static inline bool xemu_texture_packs_has_all_cubemap_faces(uint64_t hash, int *width, int *height)
{ (void)hash; (void)width; (void)height; return false; }
static inline const char *xemu_texture_packs_cubemap_face_name(int face) { (void)face; return NULL; }
static inline bool xemu_texture_packs_replacement_is_animated(uint64_t hash, const char *variant)
{ (void)hash; (void)variant; return false; }
static inline const char *xemu_texture_packs_get_shader_path(uint64_t hash, const char *variant)
{ (void)hash; (void)variant; return NULL; }
static inline bool xemu_texture_packs_get_file_stamp(
    const char *path, XemuTexturePacksFileStamp *stamp)
{ (void)path; (void)stamp; return false; }
static inline int64_t xemu_texture_packs_anim_now_us(void) { return 0; }
static inline int xemu_texture_packs_animated_frame_index(uint64_t hash, const char *variant, int64_t now_us)
{ (void)hash; (void)variant; (void)now_us; return -1; }
static inline const uint8_t *xemu_texture_packs_animated_frame_pixels(uint64_t hash, const char *variant,
                                                                      int frame, int *width, int *height)
{ (void)hash; (void)variant; (void)frame; (void)width; (void)height; return NULL; }
static inline void xemu_texture_packs_set_backend(XemuTexturePacksBackend backend) { (void)backend; }
static inline XemuTexturePacksBackend xemu_texture_packs_get_backend(void) { return XEMU_TEXTURE_PACKS_BACKEND_NONE; }
static inline void xemu_texture_packs_request_cache_flush(void) {}
static inline bool xemu_texture_packs_consume_flush_request(void) { return false; }
static inline void xemu_texture_packs_renderer_sync(void (*flush_backend)(void)) { (void)flush_backend; }
static inline void xemu_texture_packs_get_material_config(XemuTexturePacksMaterialConfig *out_config) {
    if (out_config) {
        out_config->enabled = false;
        out_config->flip_normal_y = false;
        out_config->normal_strength = 0.0f;
        out_config->ambient_strength = 0.0f;
        out_config->diffuse_strength = 0.0f;
        out_config->specular_strength = 0.0f;
        out_config->specular_power = 0.0f;
        out_config->parallax_scale = 0.0f;
        out_config->ao_strength = 0.0f;
        out_config->light_mode = 0;
        out_config->light_dir[0] = 0.0f;
        out_config->light_dir[1] = 0.0f;
        out_config->light_dir[2] = 0.0f;
    }
}
static inline void xemu_texture_packs_set_material_config(const XemuTexturePacksMaterialConfig *config) { (void)config; }
static inline bool xemu_texture_packs_material_enhancement_enabled(void) { return false; }
static inline bool xemu_texture_packs_material_sidecars_present(uint64_t hash) { (void)hash; return false; }
static inline uint64_t xemu_texture_packs_material_config_revision(void) { return 0; }
static inline bool xemu_texture_packs_material_camera_tracking_needed(void) { return false; }
static inline bool xemu_texture_packs_material_bound_hash(void *pgraph, int stage, uint64_t *out_hash)
{ (void)pgraph; (void)stage; if (out_hash) *out_hash = 0; return false; }
static inline void xemu_texture_packs_material_set_hash_view_light(uint64_t hash, const float dir[3]) { (void)hash; (void)dir; }
static inline uint64_t xemu_texture_packs_material_light_revision(void) { return 0; }
typedef void (*XemuTexturePacksMaterialDrawRefreshFn)(void *opaque, int stage, uint64_t hash);
static inline void xemu_texture_packs_material_set_draw_refresh_callback(XemuTexturePacksMaterialDrawRefreshFn callback) { (void)callback; }
static inline void xemu_texture_packs_material_refresh_draw(void *opaque, int stage, uint64_t hash) { (void)opaque; (void)stage; (void)hash; }
static inline void xemu_texture_packs_material_get_hash_view_light(uint64_t hash, float dir[3], uint64_t *revision) { (void)hash; if (dir) { dir[0] = 0.0f; dir[1] = 0.0f; dir[2] = 1.0f; } if (revision) { *revision = 0; } }
#endif
#ifdef __cplusplus
}
#endif
#endif

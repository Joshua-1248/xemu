/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * NV2A OpenGL texture-pack boundary. Renderer lineage and attribution are
 * preserved in CREDITS.md and THIRD_PARTY_NOTICES.md.
 */
#ifndef XEMU_FEATURES_TEXTURE_PACKS_GL_H
#define XEMU_FEATURES_TEXTURE_PACKS_GL_H
#include "config-host.h"
#include <stdbool.h>
#include <stdint.h>
struct TextureBinding;
#ifdef CONFIG_XEMU_FEATURE_TEXTURE_PACKS
bool xemu_texture_packs_gl_try_upload_replacement(uint64_t hash, bool gen_mipmaps);
bool xemu_texture_packs_gl_try_upload_replacement_target(uint64_t hash, const char *variant,
                                                         unsigned int gl_target, bool gen_mipmaps);
bool xemu_texture_packs_gl_upload_animated_frame(uint64_t hash, const char *variant,
                                                 unsigned int gl_target, bool full_upload,
                                                 bool regen_mips, int64_t now_us, int *out_frame);
void xemu_texture_packs_gl_maybe_dump_guest32(uint64_t hash, unsigned int gl_target, int level,
                                              unsigned int width, unsigned int height,
                                              unsigned int row_stride, uint32_t guest_color_format,
                                              const uint8_t *data);
bool xemu_texture_packs_gl_try_replace_bound_texture(uint64_t hash, unsigned int gl_target,
                                                      int guest_levels, bool *out_animated);
void xemu_texture_packs_gl_binding_created(struct TextureBinding *binding, uint64_t hash,
                                           int guest_width, int guest_height, bool animated,
                                           bool animated_has_mips);
void xemu_texture_packs_gl_binding_destroy(struct TextureBinding *binding);
bool xemu_texture_packs_gl_apply_sampler_override(struct TextureBinding *binding);
void xemu_texture_packs_gl_refresh_binding(struct TextureBinding *binding);
void xemu_texture_packs_gl_refresh_dynamic(void);
bool xemu_texture_packs_gl_bound_hash(void *pgraph, int stage, uint64_t *out_hash);
#else
static inline bool xemu_texture_packs_gl_try_upload_replacement(uint64_t hash, bool gen_mipmaps)
{ (void)hash; (void)gen_mipmaps; return false; }
static inline bool xemu_texture_packs_gl_try_upload_replacement_target(uint64_t hash, const char *variant,
                                                                       unsigned int gl_target, bool gen_mipmaps)
{ (void)hash; (void)variant; (void)gl_target; (void)gen_mipmaps; return false; }
static inline bool xemu_texture_packs_gl_upload_animated_frame(uint64_t hash, const char *variant,
                                                               unsigned int gl_target, bool full_upload,
                                                               bool regen_mips, int64_t now_us, int *out_frame)
{ (void)hash; (void)variant; (void)gl_target; (void)full_upload; (void)regen_mips; (void)now_us; (void)out_frame; return false; }
static inline void xemu_texture_packs_gl_maybe_dump_guest32(uint64_t hash, unsigned int gl_target, int level,
                                                            unsigned int width, unsigned int height,
                                                            unsigned int row_stride, uint32_t guest_color_format,
                                                            const uint8_t *data)
{ (void)hash; (void)gl_target; (void)level; (void)width; (void)height; (void)row_stride; (void)guest_color_format; (void)data; }
static inline bool xemu_texture_packs_gl_try_replace_bound_texture(uint64_t hash, unsigned int gl_target,
                                                                    int guest_levels, bool *out_animated)
{ (void)hash; (void)gl_target; (void)guest_levels; if (out_animated) *out_animated = false; return false; }
static inline void xemu_texture_packs_gl_binding_created(struct TextureBinding *binding, uint64_t hash,
                                                         int guest_width, int guest_height, bool animated,
                                                         bool animated_has_mips)
{ (void)binding; (void)hash; (void)guest_width; (void)guest_height; (void)animated; (void)animated_has_mips; }
static inline void xemu_texture_packs_gl_binding_destroy(struct TextureBinding *binding) { (void)binding; }
static inline bool xemu_texture_packs_gl_apply_sampler_override(struct TextureBinding *binding) { (void)binding; return false; }
static inline void xemu_texture_packs_gl_refresh_binding(struct TextureBinding *binding) { (void)binding; }
static inline void xemu_texture_packs_gl_refresh_dynamic(void) {}
static inline bool xemu_texture_packs_gl_bound_hash(void *pgraph, int stage, uint64_t *out_hash)
{ (void)pgraph; (void)stage; if (out_hash) *out_hash = 0; return false; }
#endif
#endif

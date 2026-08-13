/*
 * Copyright (c) 2026 Joshua-1248
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_GL_TEXTURE_IO_H
#define HW_XBOX_NV2A_PGRAPH_GL_TEXTURE_IO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve per-title dump/replacement directories from the currently loaded
 * XBE certificate title ID, creating them if necessary.
 *
 * Cheap to call repeatedly: work is only performed when the resolved title
 * ID differs from the cached one.
 */
void nv2a_texture_io_refresh_paths(void);

/* True when a title has been resolved and the directories are usable. */
bool nv2a_texture_io_ready(void);

/*
 * Write decoded 32bpp texture data to
 *   <base>/textures/<TITLEID>/dumps/<hash>.png
 *
 * Does nothing if the file already exists, so repeatedly re-uploaded
 * textures do not cause repeated disk writes.
 */
void nv2a_texture_io_dump(uint64_t hash, unsigned int width,
                          unsigned int height, const uint8_t *rgba_data);

/*
 * Look for <base>/textures/<TITLEID>/replacements/<hash>.png
 *
 * On success uploads the image into the currently bound GL texture
 * (generating a mip chain when gen_mipmaps is set) and returns true.
 * On failure returns false; the caller must perform the normal upload.
 */
bool nv2a_texture_io_try_upload_replacement(uint64_t hash, bool gen_mipmaps);

/*
 * Rescan the replacement directory tree and rebuild the hash -> path index.
 * Called automatically when paths are resolved; call again to pick up files
 * added or edited while running.
 */
void nv2a_texture_io_rebuild_replacement_index(void);

/*
 * Rescan the dump directory. Call when dumping is enabled so that files
 * deleted from disk get dumped again instead of being suppressed by a
 * stale in-memory set.
 */
void nv2a_texture_io_rebuild_dump_index(void);

/*
 * Query a replacement's dimensions without decoding the whole image.
 * Returns false when no replacement exists for this hash.
 */
bool nv2a_texture_io_get_replacement_size(uint64_t hash, int *width,
                                          int *height);

/*
 * Decode a replacement to tightly packed RGBA8. Returns NULL when absent or
 * undecodable. Free the result with nv2a_texture_io_free_pixels().
 */
uint8_t *nv2a_texture_io_load_replacement_rgba(uint64_t hash, int *width,
                                               int *height);

void nv2a_texture_io_free_pixels(uint8_t *pixels);

/*
 * Variant-aware forms. `variant` is NULL for a plain texture, "posx".."negz"
 * for cubemap faces, or "mip1".."mipN" for mip levels. Files are named
 * <hash>[_<variant>].png.
 */
void nv2a_texture_io_dump_variant(uint64_t hash, const char *variant,
                                  unsigned int width, unsigned int height,
                                  const uint8_t *rgba_data);

bool nv2a_texture_io_get_replacement_size_variant(uint64_t hash,
                                                  const char *variant,
                                                  int *width, int *height);

uint8_t *nv2a_texture_io_load_replacement_rgba_variant(uint64_t hash,
                                                       const char *variant,
                                                       int *width,
                                                       int *height);

/*
 * True only when all six faces exist AND share dimensions. A partial or
 * mismatched set is rejected so the guest cubemap is used unmodified.
 */
bool nv2a_texture_io_has_all_cubemap_faces(uint64_t hash, int *width,
                                           int *height);

/* "posx", "negx", "posy", "negy", "posz", "negz" for face 0..5. */
const char *nv2a_texture_io_cubemap_face_name(int face);

/* GL: upload into an explicit target (2D or a cube map face). */
bool nv2a_texture_io_try_upload_replacement_target(uint64_t hash,
                                                   const char *variant,
                                                   unsigned int gl_target,
                                                   bool gen_mipmaps);

/*
 * Animated replacements (.gif, .webp).
 *
 * Detected purely by file extension on the resolved replacement path, as
 * requested -- the replacement index already resolves <hash>[_variant] to a
 * path during the directory scan, so this is a suffix check, not a content
 * sniff. Cubemap faces and mip levels are not supported for animated
 * replacements (matches the existing cubemap-must-be-a-flat-PNG-set
 * assumption); only the plain/variant=NULL case is checked.
 *
 * Frames are decoded once, in full, on first use and cached in memory for
 * the lifetime of the replacement index (freed and re-decoded if the index
 * is rebuilt). This trades memory for simplicity: a texture pack's worth of
 * decoded GIF/WebM frames is expected to be small relative to VRAM-side
 * texture data, and re-decoding every bind would be far too slow.
 */
bool nv2a_texture_io_replacement_is_animated(uint64_t hash,
                                             const char *variant);

/*
 * Upload the current frame of an animated replacement into the currently
 * bound GL texture, given an absolute monotonic timestamp in microseconds
 * (pass g_get_monotonic_time()). Used both for the initial upload (in which
 * case gen_mipmaps may be honoured) and for subsequent per-bind refreshes
 * (call with gen_mipmaps=false; mip levels beyond level 0 are intentionally
 * left stale on animated textures, matching typical replacement pack use
 * where mips are not shipped for these anyway).
 *
 * *out_frame is updated to the frame index now on screen. Returns false if
 * there is no animated replacement for this hash/variant, or if decode
 * failed.
 */
/*
 * Path of a <hash>[_variant].shader procedural shader attachment, or NULL.
 * Owned by the replacement index; invalidated when that index is rebuilt.
 */
const char *nv2a_texture_io_get_shader_path(uint64_t hash,
                                            const char *variant);

/*
 * Current animation timebase in microseconds. Backed by QEMU_CLOCK_VIRTUAL,
 * so it stops while the machine is paused and resumes from the same point.
 * Pass this as now_us to the frame query and upload calls.
 */
int64_t nv2a_texture_io_anim_now_us(void);

/*
 * Frame index due at now_us, or -1 if this hash has no animated
 * replacement. No upload; use it to avoid redundant per-draw work.
 */
int nv2a_texture_io_animated_frame_index(uint64_t hash, const char *variant,
                                         int64_t now_us);

/* Borrowed RGBA8 frame pixels; do not free. NULL when absent. */
const uint8_t *nv2a_texture_io_animated_frame_pixels(uint64_t hash,
                                                     const char *variant,
                                                     int frame, int *width,
                                                     int *height);

bool nv2a_texture_io_upload_animated_frame(uint64_t hash, const char *variant,
                                           unsigned int gl_target,
                                           bool full_upload, bool regen_mips,
                                           int64_t now_us, int *out_frame);

/*
 * Active renderer backend, registered by whichever backend initialised its
 * texture cache. The GL and Vulkan renderer state pointers live in a union,
 * so the flush below must never guess which one is valid.
 */
typedef enum {
    NV2A_TEXTURE_BACKEND_NONE = 0,
    NV2A_TEXTURE_BACKEND_GL,
    NV2A_TEXTURE_BACKEND_VK,
} Nv2aTextureBackend;

void nv2a_texture_io_set_backend(Nv2aTextureBackend backend);
Nv2aTextureBackend nv2a_texture_io_get_backend(void);

/*
 * Request that every cached texture binding be dropped so bindings are
 * re-resolved against disk. Safe to call from any thread; the flush itself
 * happens on the renderer thread at the start of its next bind.
 */
void nv2a_texture_cache_flush(void);

/*
 * Renderer-thread side of the above: returns true once per request. Called
 * at the top of each backend's bind_textures().
 */
bool nv2a_texture_io_consume_flush_request(void);

/* Backend implementations; called only via the request mechanism. */
void pgraph_gl_texture_cache_flush(void);
void pgraph_vk_texture_cache_flush(void);

#ifdef __cplusplus
}
#endif

#endif

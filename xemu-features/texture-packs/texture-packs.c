/* SPDX-License-Identifier: GPL-2.0-or-later */
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

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_regs.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "xemu-xbe.h"
#include "ui/xemu-settings.h"
#include "qemu/timer.h"
#include "xemu-features/texture-packs/texture-packs.h"

/*
 * PNG encode/decode.
 *
 * stb_image.h is already vendored by xemu. stb_image_write.h is NOT (xemu
 * uses fpng for screenshots), so it must be added alongside stb_image.h --
 * see INSTALL.md step 4. We define the implementation here because no other
 * translation unit in the tree does.
 */
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/*
 * Animated replacement decode:
 *   .gif  -- stb_image (vendored); decodes every frame plus per-frame delay
 *            from memory in one call.
 *   .webp -- libwebp's demux library, which handles both animated and still
 *            WebP; a still image decodes as a single frame, so .webp always
 *            goes through this path rather than stb_image (which has no
 *            WebP support at all).
 */
#ifdef CONFIG_LIBWEBP
#include <webp/decode.h>
#include <webp/demux.h>
#endif

static XemuTexturePacksBackend g_backend = XEMU_TEXTURE_PACKS_BACKEND_NONE;

bool xemu_texture_packs_dump_enabled(void)
{
    return g_config.general.texture_dump_enabled;
}

bool xemu_texture_packs_replace_enabled(void)
{
    return g_config.general.texture_replace_enabled;
}

bool xemu_texture_packs_dump_mipmaps(void)
{
    return g_config.general.texture_dump_mipmaps;
}

bool xemu_texture_packs_should_dump_level(unsigned int level)
{
    return xemu_texture_packs_dump_enabled() &&
           (level == 0 || xemu_texture_packs_dump_mipmaps());
}

bool xemu_texture_packs_dynamic_enabled(void)
{
    return xemu_texture_packs_replace_enabled() &&
           xemu_texture_packs_has_dynamic_replacements();
}

void xemu_texture_packs_set_backend(XemuTexturePacksBackend backend)
{
    g_backend = backend;
}

XemuTexturePacksBackend xemu_texture_packs_get_backend(void)
{
    return g_backend;
}

/*
 * Cache flush is *requested* here and performed by the renderer thread at
 * the top of its next bind_textures() call.
 *
 * Flushing directly from the UI thread is unsafe even with the renderer
 * lock held: the renderer keeps live pointers in texture_bindings[] for the
 * frame in flight, and dropping them mid-frame trips assertions further
 * down the pipeline. Deferring to the start of a bind is both on the right
 * thread and at a point where those pointers are about to be repopulated.
 */
static bool g_flush_requested;

static void rebuild_replacement_index_now(void);
static void rebuild_dump_index_now(void);

static uint32_t g_cached_title_id;
static bool g_paths_valid;
static char *g_dump_dir;
static char *g_replace_dir;

/*
 * THREADING.
 *
 * g_replace_index, g_dumped_set, g_anim_cache, g_shader_index, g_dump_dir and
 * g_replace_dir are read by the NV2A pfifo thread from inside bind_textures.
 * That thread holds d->pfifo.lock / d->pgraph.lock, NOT the BQL, so it runs
 * concurrently with the UI thread.
 *
 * Every mutation therefore happens on the pfifo thread, at the single point
 * where xemu_texture_packs_consume_flush_request() is called -- the same place,
 * and for the same reason, that the texture cache flush is already deferred
 * to. The UI thread only ever stages values here and raises a request flag.
 *
 * Without this, pressing the reload hotkey or changing a directory in the
 * settings UI calls g_hash_table_destroy() on tables the pfifo thread is
 * walking, and frees decoded animation frames whose pixel pointers it has
 * already handed out. That is a use-after-free reachable in ordinary use.
 */
static bool g_reload_requested;

/* Handoff slot. g_pending_staged is cleared once the pfifo thread adopts. */
static bool g_pending_staged;
static bool g_pending_valid;
static uint32_t g_pending_title_id;

/*
 * What was last staged, kept AFTER adoption clears the handoff slot. The
 * "has anything changed?" test below must compare against this, not against
 * the handoff slot -- otherwise every poll after an adoption looks like a
 * change and re-stages, which rescans the whole replacement directory tree
 * and flushes the texture cache twice a second, forever.
 */
static bool g_staged_known;
static bool g_staged_valid;
static uint32_t g_staged_title_id;
static char *g_pending_dump_dir;
static char *g_pending_replace_dir;


void xemu_texture_packs_request_cache_flush(void)
{
    g_flush_requested = true;
}

bool xemu_texture_packs_consume_flush_request(void)
{
    /*
     * Called at the top of pgraph_{gl,vk}_bind_textures, i.e. on the pfifo
     * thread, before any binding hands out a pointer into the animation
     * cache. This is the only place the indices and path strings are
     * mutated -- see the THREADING note above.
     */
    if (g_reload_requested) {
        g_reload_requested = false;

        if (g_pending_staged) {
            g_pending_staged = false;

            g_free(g_dump_dir);
            g_free(g_replace_dir);
            g_dump_dir = g_pending_dump_dir;
            g_replace_dir = g_pending_replace_dir;
            g_pending_dump_dir = NULL;
            g_pending_replace_dir = NULL;

            g_paths_valid = g_pending_valid;
            g_cached_title_id = g_pending_valid ? g_pending_title_id : 0;
        }

        /* Also rebuilds the dump index. Safe here, unsafe anywhere else. */
        rebuild_replacement_index_now();

        /* Cached bindings still point at the old index; drop them. */
        g_flush_requested = true;
    }

    if (!g_flush_requested) {
        return false;
    }
    g_flush_requested = false;
    return true;
}

void xemu_texture_packs_renderer_sync(void (*flush_backend)(void))
{
    if (flush_backend != NULL && xemu_texture_packs_consume_flush_request()) {
        flush_backend();
    }
}


/*
 * Replacement index: filename stem -> full path on disk.
 *
 * Built by scanning the replacement directory tree once, so packs can be
 * organised into arbitrary subfolders. Keying on the stem (rather than the
 * bare hash) lets cubemap faces and mip levels share a hash while differing
 * by suffix. This also avoids a stat() on every texture cache miss.
 */
static GHashTable *g_replace_index;

/*
 * Set of stems already present in the dump directory. Avoids a stat() per
 * dump-enabled cache miss and survives files written in earlier sessions.
 */
static GHashTable *g_dumped_set;

/* Decoded-frame cache for animated (.gif/.webp) replacements, keyed
 * identically to g_replace_index: build_key(hash, variant). Declared here
 * so it exists before xemu_texture_packs_rebuild_replacement_index() uses it. */
static GHashTable *g_anim_cache;

/*
 * Procedural shader attachments: stem -> path of a <hash>[_variant].shader
 * file. Kept separate from g_replace_index because a shader is an attachment
 * to a hash rather than a competing image format -- a hash may have a shader
 * and an image (used as iChannel0), a shader alone (fully procedural), or an
 * image alone.
 */
static GHashTable *g_shader_index;

/* True when the current replacement pack contains at least one animated
 * image or procedural shader. Renderer bind paths use this to avoid walking
 * the entire texture cache for static-only packs. */
static bool g_has_dynamic_replacements;

bool xemu_texture_packs_has_dynamic_replacements(void)
{
    return g_has_dynamic_replacements;
}

void xemu_texture_packs_refresh_paths(void)
{
    /* With both features off there is no per-title texture work to maintain.
     * This function is called once per host UI frame, so make the completely
     * inactive case literally a couple of config reads. Paths are resolved on
     * the first frame after either feature is enabled. */
    if (!g_config.general.texture_dump_enabled &&
        !g_config.general.texture_replace_enabled) {
        return;
    }

    /*
     * Resolving the running title requires CPU-state synchronization and guest
     * memory reads. This function is called once per host UI frame, but the
     * title cannot change without a disc swap/reset, so 2 Hz identification is
     * ample and avoids putting guest inspection into the ordinary frame path.
     * Directory edits are still noticed within the same short interval.
     */
    static int64_t last_identify_us;
    static bool identified_once;
    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (identified_once && now_us >= last_identify_us &&
        (now_us - last_identify_us) < 500000) {
        return;
    }
    last_identify_us = now_us;
    identified_once = true;

    uint32_t title_id = 0;
    if (!xemu_get_xbe_title_id(&title_id)) {
        if (!g_staged_known || g_staged_valid) {
            g_free(g_pending_dump_dir);
            g_free(g_pending_replace_dir);
            g_pending_dump_dir = NULL;
            g_pending_replace_dir = NULL;
            g_pending_valid = false;
            g_pending_staged = true;
            g_staged_known = true;
            g_staged_valid = false;
            g_reload_requested = true;
        }
        return;
    }

    /*
     * Re-resolve when either the title or a configured directory changes,
     * so the settings UI takes effect without a restart.
     */
    static char *cached_dump_root;
    static char *cached_repl_root;
    const char *cur_dump_root = g_config.general.texture_dump_dir ?
                                    g_config.general.texture_dump_dir : "";
    const char *cur_repl_root = g_config.general.texture_replace_dir ?
                                    g_config.general.texture_replace_dir : "";

    bool dirs_changed =
        cached_dump_root == NULL || cached_repl_root == NULL ||
        strcmp(cached_dump_root, cur_dump_root) != 0 ||
        strcmp(cached_repl_root, cur_repl_root) != 0;

    /*
     * Compared against the STAGED state, not the live state: the pfifo thread
     * may not have adopted it yet (nothing is drawing, say), and re-staging
     * every 500 ms until it does would leak the previous strings.
     */
    if (g_staged_known && g_staged_valid &&
        title_id == g_staged_title_id && !dirs_changed) {
        return;
    }

    g_free(cached_dump_root);
    g_free(cached_repl_root);
    cached_dump_root = g_strdup(cur_dump_root);
    cached_repl_root = g_strdup(cur_repl_root);

    g_free(g_pending_dump_dir);
    g_free(g_pending_replace_dir);
    g_pending_dump_dir = NULL;
    g_pending_replace_dir = NULL;

    char title_hex[9];
    snprintf(title_hex, sizeof(title_hex), "%08X", title_id);

    const char *base = xemu_settings_get_base_path();

    /*
     * A user-specified directory replaces the default root but keeps the
     * per-title subfolder, so one directory can hold packs for many games.
     */
    const char *dump_root = g_config.general.texture_dump_dir;
    const char *repl_root = g_config.general.texture_replace_dir;

    if (dump_root && dump_root[0]) {
        g_pending_dump_dir =
            g_build_filename(dump_root, title_hex, "dumps", NULL);
    } else {
        g_pending_dump_dir =
            g_build_filename(base, "textures", title_hex, "dumps", NULL);
    }

    if (repl_root && repl_root[0]) {
        g_pending_replace_dir =
            g_build_filename(repl_root, title_hex, "replacements", NULL);
    } else {
        g_pending_replace_dir =
            g_build_filename(base, "textures", title_hex, "replacements", NULL);
    }

    if (g_mkdir_with_parents(g_pending_dump_dir, 0755) != 0 ||
        g_mkdir_with_parents(g_pending_replace_dir, 0755) != 0) {
        fprintf(stderr,
                "nv2a: texture-io: could not create directories for %s\n",
                title_hex);
        g_free(g_pending_dump_dir);
        g_free(g_pending_replace_dir);
        g_pending_dump_dir = NULL;
        g_pending_replace_dir = NULL;
        g_pending_valid = false;
        g_pending_staged = true;
        g_staged_known = true;
        g_staged_valid = false;
        g_reload_requested = true;
        return;
    }

    g_pending_title_id = title_id;
    g_pending_valid = true;
    g_pending_staged = true;
    g_staged_known = true;
    g_staged_valid = true;
    g_staged_title_id = title_id;
    g_reload_requested = true;

    fprintf(stderr, "nv2a: texture-io: title %s\n  dumps: %s\n  repl:  %s\n",
            title_hex, g_pending_dump_dir, g_pending_replace_dir);
}

bool xemu_texture_packs_ready(void)
{
    return g_paths_valid;
}

/*
 * Format preference when several replacement files share a stem. Higher
 * wins. WebP first: lossless, alpha, and far smaller than GIF, which is
 * stuck with a 256-colour palette.
 */
static int path_priority(const char *path)
{
    if (g_str_has_suffix(path, ".webp")) {
        return 3;
    }
    if (g_str_has_suffix(path, ".gif")) {
        return 2;
    }
    return 1; /* .png */
}

static void scan_hashed_pngs(GHashTable *table, bool store_path,
                             const char *dir, int depth)
{
    /* Guard against symlink loops and pathologically deep trees. */
    if (depth > 16) {
        return;
    }

    GDir *d = g_dir_open(dir, 0, NULL);
    if (d == NULL) {
        return;
    }

    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        char *full = g_build_filename(dir, name, NULL);

        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            scan_hashed_pngs(table, store_path, full, depth + 1);
            g_free(full);
            continue;
        }

        /*
         * Accept <16 hex digits>[_variant].{webp,gif,png} anywhere in
         * the tree. The key is the filename stem, so cubemap faces and mip
         * levels share a hash but differ by suffix. Files that do not start
         * with 16 hex digits are ignored, letting packs carry readmes and
         * source files.
         *
         * When several files share a stem the highest-priority format wins
         * rather than whichever the directory walk happened to reach first:
         * WebP is preferred because it is lossless and compresses far better
         * than GIF, which is limited to a 256-colour palette.
         */
        size_t len = strlen(name);

        /*
         * Shader attachments go in their own index and never compete with
         * image formats for the replacement slot.
         */
        if (store_path && g_str_has_suffix(name, ".shader") && len >= 23) {
            bool shader_hex = true;
            for (int i = 0; i < 16; i++) {
                if (!g_ascii_isxdigit(name[i])) {
                    shader_hex = false;
                    break;
                }
            }

            if (shader_hex && (name[16] == '.' || name[16] == '_')) {
                if (g_shader_index == NULL) {
                    g_shader_index = g_hash_table_new_full(
                        g_str_hash, g_str_equal, g_free, g_free);
                }
                char *shader_stem = g_ascii_strdown(name, len - 7);
                g_hash_table_insert(g_shader_index, shader_stem, full);
                g_has_dynamic_replacements = true;
                continue; /* table owns `full` */
            }
        }

        int ext_len = 0;
        int priority = 0;

        if (g_str_has_suffix(name, ".webp")) {
#ifdef CONFIG_LIBWEBP
            ext_len = 5;
            priority = 3;
#else
            /*
             * No WebP support in this build. Skip the file entirely rather
             * than indexing it: if a pack ships both foo.webp and foo.png,
             * indexing the webp would win on priority and then fail to
             * decode, losing a replacement that would otherwise have worked.
             */
            g_free(full);
            continue;
#endif
        } else if (g_str_has_suffix(name, ".gif")) {
            ext_len = 4;
            priority = 2;
        } else if (g_str_has_suffix(name, ".png")) {
            ext_len = 4;
            priority = 1;
        }

        if (ext_len && len >= (size_t)(16 + ext_len)) {
            bool is_hex = true;
            for (int i = 0; i < 16; i++) {
                if (!g_ascii_isxdigit(name[i])) {
                    is_hex = false;
                    break;
                }
            }

            if (is_hex && (name[16] == '.' || name[16] == '_')) {
                char *stem = g_ascii_strdown(name, len - ext_len);

                if (store_path) {
                    if (priority >= 2) {
                        g_has_dynamic_replacements = true;
                    }
                    const char *existing = g_hash_table_lookup(table, stem);

                    if (existing != NULL) {
                        if (path_priority(existing) >= priority) {
                            /* Keep what we have; this file loses. */
                            g_free(stem);
                            g_free(full);
                            continue;
                        }
                        fprintf(stderr,
                                "nv2a: texture-io: %s superseded by %s\n",
                                existing, full);
                    }

                    /* Replaces any lower-priority entry; table frees both. */
                    g_hash_table_insert(table, stem, full);
                    continue; /* table owns `full` now */
                }

                if (g_hash_table_contains(table, stem)) {
                    g_free(stem);
                    g_free(full);
                    continue;
                }

                g_hash_table_insert(table, stem, NULL);
            }
        }

        g_free(full);
    }

    g_dir_close(d);
}

/*
 * Rescan the dump directory. Called when paths change and whenever dumping
 * is switched on, so that files deleted from disk are dumped again rather
 * than being suppressed by a stale in-memory set.
 */
static void rebuild_dump_index_now(void)
{
    if (g_dumped_set != NULL) {
        g_hash_table_destroy(g_dumped_set);
        g_dumped_set = NULL;
    }

    if (!g_paths_valid || !g_config.general.texture_dump_enabled) {
        return;
    }

    g_dumped_set = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, NULL);

    scan_hashed_pngs(g_dumped_set, false, g_dump_dir, 0);

    fprintf(stderr, "nv2a: texture-io: %u texture(s) already dumped\n",
            g_hash_table_size(g_dumped_set));
}

static void rebuild_replacement_index_now(void)
{
    g_has_dynamic_replacements = false;

    if (g_replace_index != NULL) {
        g_hash_table_destroy(g_replace_index);
        g_replace_index = NULL;
    }

    if (!g_paths_valid) {
        return;
    }

    /* A replacement index is only useful while replacements are active, or
     * while dumping is configured to skip textures that have replacements. */
    const bool need_replace_index =
        g_config.general.texture_replace_enabled ||
        (g_config.general.texture_dump_enabled &&
         g_config.general.texture_dump_skip_replaced);

    if (need_replace_index) {
        g_replace_index = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
    }

    /* Paths are about to be re-resolved, so drop any cached decoded frames
     * for the previous set of files -- otherwise a replaced/removed .gif or
     * .webp would keep animating from stale in-memory frames. */
    if (g_anim_cache != NULL) {
        g_hash_table_destroy(g_anim_cache);
        g_anim_cache = NULL;
    }

    if (g_shader_index != NULL) {
        g_hash_table_destroy(g_shader_index);
        g_shader_index = NULL;
    }

    if (g_replace_index != NULL) {
        scan_hashed_pngs(g_replace_index, true, g_replace_dir, 0);
        fprintf(stderr,
                "nv2a: texture-io: indexed %u replacement texture(s)\n",
                g_hash_table_size(g_replace_index));
    }

    rebuild_dump_index_now();
}

/* Public entry points. Callable from any thread: they only raise the flag,
 * and the pfifo thread does the work in consume_flush_request(). */
void xemu_texture_packs_rebuild_replacement_index(void)
{
    g_reload_requested = true;
}

void xemu_texture_packs_rebuild_dump_index(void)
{
    g_reload_requested = true;
}

/*
 * Variant key: "<hash>" for a plain texture, "<hash>_<variant>" for cubemap
 * faces ("posx".."negz") and mip levels ("mip1"..).
 */
#define TEXTURE_KEY_STACK_SIZE 64

/* Most variants are tiny (posx, mip1, ...), so lookup keys belong on the
 * stack rather than on GLib's heap in a per-draw path. The heap fallback
 * preserves the old behavior for an unexpectedly long variant. */
static const char *build_key(uint64_t hash, const char *variant,
                             char stack[TEXTURE_KEY_STACK_SIZE], char **heap)
{
    int n;
    if (variant && variant[0]) {
        n = snprintf(stack, TEXTURE_KEY_STACK_SIZE,
                     "%016" PRIx64 "_%s", hash, variant);
    } else {
        n = snprintf(stack, TEXTURE_KEY_STACK_SIZE, "%016" PRIx64, hash);
    }
    if (n >= 0 && n < TEXTURE_KEY_STACK_SIZE) {
        return stack;
    }
    *heap = variant && variant[0]
        ? g_strdup_printf("%016" PRIx64 "_%s", hash, variant)
        : g_strdup_printf("%016" PRIx64, hash);
    return *heap;
}

static char *build_path(const char *dir, uint64_t hash, const char *variant)
{
    char key_stack[TEXTURE_KEY_STACK_SIZE];
    g_autofree char *key_heap = NULL;
    const char *key = build_key(hash, variant, key_stack, &key_heap);
    g_autofree char *name = g_strdup_printf("%s.png", key);
    return g_build_filename(dir, name, NULL);
}

void xemu_texture_packs_dump_variant(uint64_t hash, const char *variant,
                                  unsigned int width, unsigned int height,
                                  const uint8_t *rgba_data)
{
    if (!g_paths_valid || rgba_data == NULL || width == 0 || height == 0) {
        return;
    }

    char key_stack[TEXTURE_KEY_STACK_SIZE];
    g_autofree char *key_heap = NULL;
    const char *key = build_key(hash, variant, key_stack, &key_heap);

    /* Already dumped, this session or a previous one. */
    if (g_dumped_set != NULL && g_hash_table_contains(g_dumped_set, key)) {
        return;
    }

    /*
     * Optionally skip textures that already have a replacement. Keeps the
     * dump directory focused on work still outstanding. Disable to keep an
     * exhaustive inventory of every texture the title uses.
     */
    if (g_config.general.texture_dump_skip_replaced &&
        g_replace_index != NULL &&
        g_hash_table_contains(g_replace_index, key)) {
        return;
    }

    char *path = build_path(g_dump_dir, hash, variant);

    if (!stbi_write_png(path, (int)width, (int)height, 4, rgba_data,
                        (int)(width * 4))) {
        fprintf(stderr, "nv2a: texture-io: failed writing %s\n", path);
        g_free(path);
        return;
    }

    g_free(path);

    if (g_dumped_set != NULL) {
        g_hash_table_insert(g_dumped_set, g_strdup(key), NULL);
    }
}

void xemu_texture_packs_dump(uint64_t hash, unsigned int width,
                          unsigned int height, const uint8_t *rgba_data)
{
    xemu_texture_packs_dump_variant(hash, NULL, width, height, rgba_data);
}

/*
 * Xbox texture formats do not all live in memory as RGBA bytes. In
 * particular A8R8G8B8/X8R8G8B8 are little-endian BGRA in guest memory.
 * OpenGL/Vulkan know that from their upload format, but stb_image_write does
 * not: handing those bytes straight to stbi_write_png swaps red and blue.
 *
 * Replacement images are decoded by stb_image as RGBA8, so dumps must also
 * be canonical RGBA8 or a dump->replacement round trip changes the picture.
 */
void xemu_texture_packs_dump_guest32_variant(uint64_t hash, const char *variant,
                                          unsigned int width,
                                          unsigned int height,
                                          unsigned int row_stride,
                                          unsigned int color_format,
                                          const uint8_t *pixel_data)
{
    if (pixel_data == NULL || width == 0 || height == 0) {
        return;
    }

    enum {
        DUMP_LAYOUT_RGBA,
        DUMP_LAYOUT_BGRA,
        DUMP_LAYOUT_ARGB_BYTES,
        DUMP_LAYOUT_ABGR_BYTES,
    } layout;
    bool force_opaque = false;

    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8:
        layout = DUMP_LAYOUT_BGRA;
        break;

    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8:
        layout = DUMP_LAYOUT_BGRA;
        force_opaque = true;
        break;

    /* These paths have already been expanded/converted to RGBA8. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8:
        layout = DUMP_LAYOUT_RGBA;
        break;

    /* Little-endian byte order for the remaining 32-bit packed formats. */
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8:
        layout = DUMP_LAYOUT_ARGB_BYTES; /* bytes: A,R,G,B */
        break;
    case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8:
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8:
        layout = DUMP_LAYOUT_ABGR_BYTES; /* bytes: A,B,G,R */
        break;

    default:
        /* Depth and non-32bpp formats need format-specific expansion. */
        return;
    }

    if (row_stride == 0) {
        row_stride = width * 4;
    }

    /* Avoid an allocation when the source is already tightly packed RGBA. */
    if (layout == DUMP_LAYOUT_RGBA && !force_opaque &&
        row_stride == width * 4) {
        xemu_texture_packs_dump_variant(hash, variant, width, height, pixel_data);
        return;
    }

    g_autofree uint8_t *rgba = g_malloc((size_t)width * height * 4);
    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *src = pixel_data + (size_t)y * row_stride;
        uint8_t *dst = rgba + (size_t)y * width * 4;
        for (unsigned int x = 0; x < width; x++, src += 4, dst += 4) {
            switch (layout) {
            case DUMP_LAYOUT_RGBA:
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = force_opaque ? 255 : src[3];
                break;
            case DUMP_LAYOUT_BGRA:
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = force_opaque ? 255 : src[3];
                break;
            case DUMP_LAYOUT_ARGB_BYTES:
                dst[0] = src[1];
                dst[1] = src[2];
                dst[2] = src[3];
                dst[3] = src[0];
                break;
            case DUMP_LAYOUT_ABGR_BYTES:
                dst[0] = src[3];
                dst[1] = src[2];
                dst[2] = src[1];
                dst[3] = src[0];
                break;
            }
        }
    }

    xemu_texture_packs_dump_variant(hash, variant, width, height, rgba);
}

bool xemu_texture_packs_get_replacement_size_variant(uint64_t hash,
                                                  const char *variant,
                                                  int *width, int *height)
{
    if (g_replace_index == NULL) {
        return false;
    }

    char key_stack[TEXTURE_KEY_STACK_SIZE];
    g_autofree char *key_heap = NULL;
    const char *key = build_key(hash, variant, key_stack, &key_heap);
    const char *path = g_hash_table_lookup(g_replace_index, key);
    if (path == NULL) {
        return false;
    }

    /*
     * Animated formats first: stbi_info cannot read WebP at all, and for
     * GIF it would only describe the first frame. get_animated() also caches
     * the decode, which the caller is about to need anyway.
     */
    int aw = 0, ah = 0;
    if (xemu_texture_packs_animated_frame_pixels(hash, variant, 0, &aw, &ah) !=
        NULL) {
        *width = aw;
        *height = ah;
        return true;
    }

    int w = 0, h = 0, comp = 0;
    if (!stbi_info(path, &w, &h, &comp) || w <= 0 || h <= 0) {
        return false;
    }

    *width = w;
    *height = h;
    return true;
}

bool xemu_texture_packs_get_replacement_size(uint64_t hash, int *width,
                                          int *height)
{
    return xemu_texture_packs_get_replacement_size_variant(hash, NULL, width,
                                                        height);
}

bool xemu_texture_packs_has_all_cubemap_faces(uint64_t hash, int *width,
                                           int *height)
{
    static const char *faces[6] = { "posx", "negx", "posy",
                                    "negy", "posz", "negz" };
    int w0 = 0, h0 = 0;

    for (int i = 0; i < 6; i++) {
        int w = 0, h = 0;
        if (!xemu_texture_packs_get_replacement_size_variant(hash, faces[i], &w,
                                                          &h)) {
            return false;
        }
        if (i == 0) {
            w0 = w;
            h0 = h;
        } else if (w != w0 || h != h0) {
            fprintf(stderr,
                    "nv2a: texture-io: cubemap %016" PRIx64
                    " faces differ in size; ignoring\n", hash);
            return false;
        }
    }

    *width = w0;
    *height = h0;
    return true;
}

const char *xemu_texture_packs_cubemap_face_name(int face)
{
    static const char *faces[6] = { "posx", "negx", "posy",
                                    "negy", "posz", "negz" };
    return (face >= 0 && face < 6) ? faces[face] : NULL;
}

uint8_t *xemu_texture_packs_load_replacement_rgba_variant(uint64_t hash,
                                                       const char *variant,
                                                       int *width, int *height)
{
    if (g_replace_index == NULL) {
        return NULL;
    }

    char key_stack[TEXTURE_KEY_STACK_SIZE];
    g_autofree char *key_heap = NULL;
    const char *key = build_key(hash, variant, key_stack, &key_heap);
    const char *path = g_hash_table_lookup(g_replace_index, key);
    if (path == NULL) {
        return NULL;
    }

    int w = 0, h = 0, comp = 0;
    unsigned char *pixels = stbi_load(path, &w, &h, &comp, 4);
    if (pixels == NULL) {
        fprintf(stderr, "nv2a: texture-io: could not decode %s\n", path);
        return NULL;
    }

    *width = w;
    *height = h;
    return (uint8_t *)pixels;
}

uint8_t *xemu_texture_packs_load_replacement_rgba(uint64_t hash, int *width,
                                               int *height)
{
    return xemu_texture_packs_load_replacement_rgba_variant(hash, NULL, width,
                                                         height);
}

void xemu_texture_packs_free_pixels(uint8_t *pixels)
{
    stbi_image_free(pixels);
}

/*
 * ---------------------------------------------------------------------
 * Animated replacements (.gif, .webp)
 * ---------------------------------------------------------------------
 */

typedef struct AnimReplacement {
    int width, height;
    int frame_count;
    uint8_t **frames;   /* frame_count buffers, each width*height*4 RGBA */
    int *delay_ms;      /* frame_count entries, each > 0 */
    int64_t *end_ms;    /* cumulative exclusive end time for binary lookup */
    int64_t total_ms;   /* sum of delay_ms, for wraparound */
    bool decode_failed; /* cached so a broken file isn't retried every bind */
} AnimReplacement;


static void anim_replacement_free(gpointer data)
{
    AnimReplacement *a = data;
    if (a->frames) {
        for (int i = 0; i < a->frame_count; i++) {
            g_free(a->frames[i]);
        }
        g_free(a->frames);
    }
    g_free(a->delay_ms);
    g_free(a->end_ms);
    g_free(a);
}

static bool path_has_suffix_ci(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
           g_ascii_strcasecmp(path + path_len - suffix_len, suffix) == 0;
}

static void anim_build_timeline(AnimReplacement *a)
{
    g_free(a->end_ms);
    a->end_ms = g_new(int64_t, a->frame_count);
    int64_t total = 0;
    for (int i = 0; i < a->frame_count; ++i) {
        total += MAX(a->delay_ms[i], 1);
        a->end_ms[i] = total;
    }
    a->total_ms = total;
}

static bool decode_gif(const char *path, AnimReplacement *out)
{
    gsize len = 0;
    g_autofree char *data = NULL;
    if (!g_file_get_contents(path, &data, &len, NULL)) {
        return false;
    }

    int w = 0, h = 0, frames = 0, comp = 0;
    int *delays = NULL;
    uint8_t *packed = stbi_load_gif_from_memory(
        (const stbi_uc *)data, (int)len, &delays, &w, &h, &frames, &comp, 4);

    if (packed == NULL || w <= 0 || h <= 0 || frames <= 0) {
        fprintf(stderr, "nv2a: texture-io: could not decode GIF %s\n", path);
        if (packed) {
            stbi_image_free(packed);
        }
        free(delays);
        return false;
    }

    out->width = w;
    out->height = h;
    out->frame_count = frames;
    out->frames = g_new0(uint8_t *, frames);
    out->delay_ms = g_new0(int, frames);

    size_t frame_bytes = (size_t)w * h * 4;
    for (int i = 0; i < frames; i++) {
        out->frames[i] = g_memdup2(packed + (size_t)i * frame_bytes,
                                   frame_bytes);
        /* GIF delay is hundredths of a second; stb already returns ms. A
         * delay of 0 is common (author intent: "as fast as possible") but
         * unusable as a wall-clock duration, so floor it. */
        out->delay_ms[i] = delays ? MAX(delays[i], 20) : 100;
        out->total_ms += out->delay_ms[i];
    }

    stbi_image_free(packed);
    free(delays);
    return true;
}

#ifdef CONFIG_LIBWEBP
static bool decode_webp(const char *path, AnimReplacement *out)
{
    gsize len = 0;
    g_autofree char *data = NULL;
    if (!g_file_get_contents(path, &data, &len, NULL)) {
        return false;
    }

    WebPData webp_data = { .bytes = (const uint8_t *)data, .size = len };

    WebPAnimDecoderOptions opts;
    if (!WebPAnimDecoderOptionsInit(&opts)) {
        return false;
    }
    opts.color_mode = MODE_RGBA;
    opts.use_threads = 1;

    WebPAnimDecoder *dec = WebPAnimDecoderNew(&webp_data, &opts);
    if (dec == NULL) {
        fprintf(stderr, "nv2a: texture-io: could not decode WebP %s\n", path);
        return false;
    }

    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec, &info) || info.canvas_width == 0 ||
        info.canvas_height == 0 || info.frame_count == 0) {
        WebPAnimDecoderDelete(dec);
        fprintf(stderr, "nv2a: texture-io: bad WebP info in %s\n", path);
        return false;
    }

    out->width = (int)info.canvas_width;
    out->height = (int)info.canvas_height;
    out->frame_count = (int)info.frame_count;
    out->frames = g_new0(uint8_t *, out->frame_count);
    out->delay_ms = g_new0(int, out->frame_count);

    const size_t frame_bytes = (size_t)out->width * out->height * 4;
    int prev_ms = 0;
    int decoded = 0;

    /*
     * WebPAnimDecoder yields fully composited canvases with an absolute
     * end-timestamp per frame, so per-frame duration is the difference
     * between consecutive timestamps.
     */
    while (WebPAnimDecoderHasMoreFrames(dec) && decoded < out->frame_count) {
        uint8_t *buf = NULL;
        int timestamp_ms = 0;

        if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp_ms)) {
            break;
        }

        out->frames[decoded] = g_memdup2(buf, frame_bytes);
        int delay = timestamp_ms - prev_ms;
        out->delay_ms[decoded] = MAX(delay, 20);
        out->total_ms += out->delay_ms[decoded];
        prev_ms = timestamp_ms;
        decoded++;
    }

    WebPAnimDecoderDelete(dec);

    if (decoded == 0) {
        g_free(out->frames);
        g_free(out->delay_ms);
        out->frames = NULL;
        out->delay_ms = NULL;
        out->frame_count = 0;
        fprintf(stderr, "nv2a: texture-io: WebP %s yielded no frames\n", path);
        return false;
    }

    /* Fewer frames than advertised: shrink rather than leave NULL holes. */
    out->frame_count = decoded;
    return true;
}
#else
/*
 * libwebp was not available at build time. .webp replacements are simply not
 * recognised; .gif still works, decoded by the vendored stb_image. Warn once
 * so a user with a WebP pack is told why nothing is appearing, rather than
 * being left to guess.
 */
static bool decode_webp(const char *path, AnimReplacement *out)
{
    static bool warned;
    (void)out;
    if (!warned) {
        warned = true;
        fprintf(stderr,
                "nv2a: texture-io: this build has no WebP support; ignoring "
                "%s and any other .webp replacement. Rebuild with libwebp "
                "(and libwebpdemux) to enable them.\n", path);
    }
    return false;
}
#endif /* CONFIG_LIBWEBP */

/*
 * Look up (decoding and caching on first use) the animated replacement for
 * hash/variant. Returns NULL if there is none, or if decode failed -- in
 * the latter case a sentinel with decode_failed=true is cached so repeated
 * binds don't retry a broken file every frame.
 */
static AnimReplacement *get_animated(uint64_t hash, const char *variant)
{
    if (g_replace_index == NULL) {
        return NULL;
    }

    char key_stack[TEXTURE_KEY_STACK_SIZE];
    g_autofree char *key_heap = NULL;
    const char *key = build_key(hash, variant, key_stack, &key_heap);

    /* Cached animated hits are the normal path and must not inspect the file
     * extension or allocate anything on every draw. */
    if (g_anim_cache != NULL) {
        AnimReplacement *cached = g_hash_table_lookup(g_anim_cache, key);
        if (cached != NULL) {
            return cached->decode_failed ? NULL : cached;
        }
    }

    const char *path = g_hash_table_lookup(g_replace_index, key);
    if (path == NULL) {
        return NULL;
    }

    bool is_gif = path_has_suffix_ci(path, ".gif");
    bool is_webp = path_has_suffix_ci(path, ".webp");
    if (!is_gif && !is_webp) {
        return NULL; /* static PNG, handled by the existing upload path */
    }

    if (g_anim_cache == NULL) {
        g_anim_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             anim_replacement_free);
    }

    AnimReplacement *a = g_new0(AnimReplacement, 1);
    bool ok = is_gif ? decode_gif(path, a) : decode_webp(path, a);
    a->decode_failed = !ok;

    if (ok) {
        anim_build_timeline(a);
        fprintf(stderr,
                "nv2a: texture-io: decoded animated replacement %s "
                "(%d frames, %dx%d, %" PRId64 "ms loop)\n",
                path, a->frame_count, a->width, a->height, a->total_ms);
    }

    g_hash_table_insert(g_anim_cache, g_strdup(key), a);
    return ok ? a : NULL;
}

bool xemu_texture_packs_replacement_is_animated(uint64_t hash,
                                             const char *variant)
{
    return get_animated(hash, variant) != NULL;
}

static int frame_for_time(const AnimReplacement *a, int64_t now_us)
{
    if (a->total_ms <= 0 || a->end_ms == NULL) {
        return 0;
    }
    const int64_t t = (now_us / 1000) % a->total_ms;

    int lo = 0;
    int hi = a->frame_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (t < a->end_ms[mid]) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return MIN(lo, a->frame_count - 1);
}

/*
 * Path of the shader attached to this hash, or NULL. The returned string is
 * owned by the index and is invalidated by a replacement-index rebuild.
 */
const char *xemu_texture_packs_get_shader_path(uint64_t hash, const char *variant)
{
    if (g_shader_index == NULL) {
        return NULL;
    }
    char key_stack[TEXTURE_KEY_STACK_SIZE];
    g_autofree char *key_heap = NULL;
    const char *key = build_key(hash, variant, key_stack, &key_heap);
    return g_hash_table_lookup(g_shader_index, key);
}

/*
 * Animation timebase.
 *
 * QEMU_CLOCK_VIRTUAL rather than wall-clock time: it halts while the machine
 * is paused and resumes from where it stopped, so a paused emulator freezes
 * its animated textures on the current frame and continues from that same
 * frame when resumed, instead of jumping ahead by however long the pause
 * lasted. It also keeps animation in step with the guest when emulation runs
 * fast or slow rather than drifting against real time.
 */
int64_t xemu_texture_packs_anim_now_us(void)
{
    return qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
}

/*
 * Frame index that should be on screen at now_us, or -1 when this hash has
 * no animated replacement. Cheap: no upload, no GL/VK calls. Callers use
 * this to skip redundant re-uploads, which matters a great deal because
 * bind_textures runs per draw call, not per frame.
 */
int xemu_texture_packs_animated_frame_index(uint64_t hash, const char *variant,
                                         int64_t now_us)
{
    AnimReplacement *a = get_animated(hash, variant);
    if (a == NULL) {
        return -1;
    }
    return frame_for_time(a, now_us);
}

/*
 * Borrowed pointer to a decoded RGBA8 frame. Valid until the replacement
 * index is rebuilt; do not free. Returns NULL when absent or out of range.
 * Lets the Vulkan backend feed its staging buffer without a disk reload.
 */
const uint8_t *xemu_texture_packs_animated_frame_pixels(uint64_t hash,
                                                     const char *variant,
                                                     int frame, int *width,
                                                     int *height)
{
    AnimReplacement *a = get_animated(hash, variant);
    if (a == NULL || frame < 0 || frame >= a->frame_count) {
        return NULL;
    }
    *width = a->width;
    *height = a->height;
    return a->frames[frame];
}



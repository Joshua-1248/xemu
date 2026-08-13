/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 * Copyright (c) 2026 Joshua-1248 (texture replacement, animation, shaders)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "debug.h"
#include "renderer.h"
#include <glib/gstdio.h>

#include "texture-io.h"
#include "ui/xemu-settings.h"

static TextureBinding* generate_texture(const TextureShape s, const uint8_t *texture_data, const uint8_t *palette_data, uint64_t data_hash);
static void texture_binding_destroy(gpointer data);

struct pgraph_texture_possibly_dirty_struct {
    hwaddr addr, end;
};

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node, void *opaque)
{
    struct pgraph_texture_possibly_dirty_struct *test =
        (struct pgraph_texture_possibly_dirty_struct *)opaque;

    struct TextureLruNode *tnode = container_of(node, TextureLruNode, node);
    if (tnode->binding == NULL || tnode->possibly_dirty) {
        return;
    }

    uintptr_t k_tex_addr = tnode->key.texture_vram_offset;
    uintptr_t k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
    bool overlapping = !(test->addr > k_tex_end || k_tex_addr > test->end);

    if (tnode->key.palette_length > 0) {
        uintptr_t k_pal_addr = tnode->key.palette_vram_offset;
        uintptr_t k_pal_end = k_pal_addr + tnode->key.palette_length - 1;
        overlapping |= !(test->addr > k_pal_end || k_pal_addr > test->end);
    }

    tnode->possibly_dirty |= overlapping;
}

void pgraph_gl_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    struct pgraph_texture_possibly_dirty_struct test = {
        .addr = addr,
        .end = end,
    };

    lru_visit_active(&r->texture_cache,
                     mark_textures_possibly_dirty_visitor,
                     &test);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

// Check if any of the pages spanned by the a texture are dirty.
static bool check_texture_possibly_dirty(NV2AState *d,
                                         hwaddr texture_vram_offset,
                                         unsigned int length,
                                         hwaddr palette_vram_offset,
                                         unsigned int palette_length)
{
    bool possibly_dirty = false;
    if (check_texture_dirty(d, texture_vram_offset, length)) {
        possibly_dirty = true;
        pgraph_gl_mark_textures_possibly_dirty(d, texture_vram_offset, length);
    }
    if (palette_length && check_texture_dirty(d, palette_vram_offset,
                                                     palette_length)) {
        possibly_dirty = true;
        pgraph_gl_mark_textures_possibly_dirty(d, palette_vram_offset,
                                            palette_length);
    }
    return possibly_dirty;
}

static void apply_texture_parameters(PGRAPHGLState *r,
                                     TextureBinding *binding,
                                     const BasicColorFormatInfo *f,
                                     unsigned int dimensionality,
                                     unsigned int filter,
                                     unsigned int address,
                                     bool is_bordered,
                                     uint32_t border_color,
                                     uint32_t max_anisotropy)
{
    /*
     * Shader-generated textures only have level 0: the FBO render pass has
     * nowhere to write a mip chain. Applying the guest's min filter here
     * would typically select a mipmapped mode (Doom 3's floors use
     * LINEAR_MIPMAP_LINEAR), leaving the texture mip-incomplete and making
     * it sample as undefined -- black or stale content depending on driver,
     * rather than the shader's output. Force a complete filter instead and
     * skip the rest of the guest's sampler state, which assumes a chain.
     */
    if (binding->has_shader) {
        glTexParameteri(binding->gl_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(binding->gl_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(binding->gl_target, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(binding->gl_target, GL_TEXTURE_MAX_LEVEL, 0);
        binding->min_filter = 0xFFFFFFFF;
        binding->mag_filter = 0xFFFFFFFF;
        return;
    }

    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    unsigned int lod_bias =
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS);
    unsigned int addru = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU);
    unsigned int addrv = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV);
    unsigned int addrp = GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP);

    if (f->linear) {
        /* somtimes games try to set mipmap min filters on linear textures.
             * this could indicate a bug... */
        switch (min_filter) {
        case NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD:
        case NV_PGRAPH_TEXFILTER0_MIN_BOX_TENT_LOD:
            min_filter = NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0;
            break;
        case NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD:
        case NV_PGRAPH_TEXFILTER0_MIN_TENT_TENT_LOD:
            min_filter = NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0;
            break;
        }
    }

    if (min_filter != binding->min_filter) {
        glTexParameteri(binding->gl_target, GL_TEXTURE_MIN_FILTER,
                        pgraph_texture_min_filter_gl_map[min_filter]);
        binding->min_filter = min_filter;
    }
    if (mag_filter != binding->mag_filter) {
        glTexParameteri(binding->gl_target, GL_TEXTURE_MAG_FILTER,
                        pgraph_texture_mag_filter_gl_map[mag_filter]);
        binding->mag_filter = mag_filter;
    }
    if (lod_bias != binding->lod_bias) {
        binding->lod_bias = lod_bias;
        glTexParameterf(binding->gl_target, GL_TEXTURE_LOD_BIAS,
                        pgraph_convert_lod_bias_to_float(lod_bias));
    }

    /* Texture wrapping */
    assert(addru < ARRAY_SIZE(pgraph_texture_addr_gl_map));
    if (addru != binding->addru) {
        glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_S,
                        pgraph_texture_addr_gl_map[addru]);
        binding->addru = addru;
    }
    bool needs_border_color = binding->addru == NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER;
    if (dimensionality > 1) {
        if (addrv != binding->addrv) {
            assert(addrv < ARRAY_SIZE(pgraph_texture_addr_gl_map));
            glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_T,
                            pgraph_texture_addr_gl_map[addrv]);
            binding->addrv = addrv;
        }
        needs_border_color = needs_border_color || binding->addrv == NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER;
    }
    if (dimensionality > 2) {
        if (addrp != binding->addrp) {
            assert(addrp < ARRAY_SIZE(pgraph_texture_addr_gl_map));
            glTexParameteri(binding->gl_target, GL_TEXTURE_WRAP_R,
                            pgraph_texture_addr_gl_map[addrp]);
            binding->addrp = addrp;
        }
        needs_border_color = needs_border_color || binding->addrp == NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER;
    }

    if (r->supported_extensions.texture_filter_anisotropic) {
        glTexParameterf(binding->gl_target, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        max_anisotropy);
    }

    if (!is_bordered && needs_border_color) {
        if (!binding->border_color_set || binding->border_color != border_color) {
            /* FIXME: Color channels might be wrong order */
            GLfloat gl_border_color[4];
            pgraph_argb_pack32_to_rgba_float(border_color, gl_border_color);
            glTexParameterfv(binding->gl_target, GL_TEXTURE_BORDER_COLOR,
                             gl_border_color);

            binding->border_color_set = true;
            binding->border_color = border_color;
        }
    }
}

/*
 * Reused texture bindings normally skip re-upload entirely (that's the
 * whole point of the cache). An animated replacement needs its frame
 * advanced on every bind regardless, since nothing about the guest-side
 * dirty tracking has any notion of "this texture changes on its own".
 * Must be called with the binding's GL texture already bound.
 */
/*
 * ---------------------------------------------------------------------
 * Procedural shader replacements (<hash>.shader)
 * ---------------------------------------------------------------------
 *
 * A shader file supplies a fragment shader body in the style of Shadertoy.
 * It is rendered into the binding's own texture through an FBO, so from the
 * perspective of the rest of the renderer the result is an ordinary texture
 * and nothing about nv2a's own shader generation needs to change.
 *
 * Uniforms available to the shader:
 *   float     iTime        seconds on the virtual clock (pauses with the VM)
 *   vec2      iResolution  target size in pixels
 *   int       iFrame       frames rendered since this binding was created
 *   sampler2D iChannel0    image replacement for the same hash, if present
 *   bool      iHasChannel0 whether iChannel0 holds anything
 *
 * The shader writes to `fragColor` and reads `uv` (0..1).
 */

#define TEXTURE_SHADER_MAX_SIZE 4096

static const char *texture_shader_vs_src =
    "#version 400 core\n"
    "out vec2 uv;\n"
    "void main() {\n"
    /* Fullscreen triangle from gl_VertexID; no vertex buffer needed. */
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *texture_shader_fs_prologue =
    "#version 400 core\n"
    "in vec2 uv;\n"
    "out vec4 fragColor;\n"
    "uniform float iTime;\n"
    "uniform vec2 iResolution;\n"
    "uniform int iFrame;\n"
    "uniform sampler2D iChannel0;\n"
    "uniform bool iHasChannel0;\n"
    "#line 1\n";

/*
 * Compile a user shader. Errors are reported once, at load, and leave the
 * binding without a shader rather than failing the texture entirely -- a bad
 * shader file should not stop the game from rendering.
 */
static GLuint compile_texture_shader(const char *path)
{
    g_autofree char *body = NULL;
    gsize body_len = 0;

    if (!g_file_get_contents(path, &body, &body_len, NULL)) {
        fprintf(stderr, "nv2a: texture-io: could not read shader %s\n", path);
        return 0;
    }

    g_autofree char *fs_src =
        g_strconcat(texture_shader_fs_prologue, body, NULL);

    GLint ok = GL_FALSE;
    char log[2048];

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &texture_shader_vs_src, NULL);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *fs_ptr = fs_src;
    glShaderSource(fs, 1, &fs_ptr, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);

    if (ok != GL_TRUE) {
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        fprintf(stderr, "nv2a: texture-io: shader %s failed to compile:\n%s\n",
                path, log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (ok != GL_TRUE) {
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "nv2a: texture-io: shader %s failed to link:\n%s\n",
                path, log);
        glDeleteProgram(program);
        return 0;
    }

    fprintf(stderr, "nv2a: texture-io: loaded shader %s\n", path);
    return program;
}

/*
 * Set up a binding's shader: compile it, size the render target, capture any
 * image replacement as iChannel0, and build the FBO. Called once, at binding
 * creation. Leaves has_shader false on any failure.
 */
static void setup_texture_shader(TextureBinding *binding, uint64_t hash,
                                 int guest_width, int guest_height)
{
    const char *path = nv2a_texture_io_get_shader_path(hash, NULL);
    if (path == NULL) {
        return;
    }

    GLuint program = compile_texture_shader(path);
    if (program == 0) {
        return;
    }

    /*
     * Target size: the image replacement's size when one exists (so a shader
     * distorting a high-res replacement keeps that resolution), otherwise the
     * guest texture's own size.
     */
    int w = guest_width, h = guest_height;
    int rw = 0, rh = 0;
    bool have_source = nv2a_texture_io_get_replacement_size(hash, &rw, &rh);

    if (have_source && rw > 0 && rh > 0) {
        w = rw;
        h = rh;
    }

    w = CLAMP(w, 1, TEXTURE_SHADER_MAX_SIZE);
    h = CLAMP(h, 1, TEXTURE_SHADER_MAX_SIZE);

    /*
     * iChannel0: the replacement image is moved into its own texture,
     * because gl_texture becomes the shader's render target and a texture
     * cannot be sampled and written in the same pass.
     */
    GLuint src_tex = 0;
    bool src_animated = nv2a_texture_io_replacement_is_animated(hash, NULL);

    if (have_source) {
        int sw = 0, sh = 0;
        uint8_t *pixels = NULL;
        const uint8_t *frame_pixels = NULL;

        if (src_animated) {
            /*
             * Borrowed from the decoded frame cache -- must not be freed,
             * and covers WebP, which stbi_load cannot read at all.
             */
            frame_pixels = nv2a_texture_io_animated_frame_pixels(
                hash, NULL, 0, &sw, &sh);
        } else {
            pixels = nv2a_texture_io_load_replacement_rgba(hash, &sw, &sh);
            frame_pixels = pixels;
        }

        if (frame_pixels != NULL) {
            glGenTextures(1, &src_tex);
            glBindTexture(GL_TEXTURE_2D, src_tex);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, frame_pixels);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }

        if (pixels != NULL) {
            nv2a_texture_io_free_pixels(pixels);
        }
    }

    /* Allocate the render target at the chosen size. */
    glBindTexture(GL_TEXTURE_2D, binding->gl_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    GLuint fbo = 0;
    GLint prev_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, binding->gl_texture, 0);

    GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_fbo);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,
                "nv2a: texture-io: shader FBO incomplete (0x%x) for %s\n",
                status, path);
        glDeleteFramebuffers(1, &fbo);
        glDeleteProgram(program);
        if (src_tex) {
            glDeleteTextures(1, &src_tex);
        }
        return;
    }

    binding->has_shader = true;
    binding->shader_program = program;
    binding->shader_fbo = fbo;
    binding->shader_src = src_tex;
    binding->shader_width = w;
    binding->shader_height = h;
    binding->shader_frame = 0;
    binding->shader_last_us = INT64_MIN;
    binding->shader_src_animated = src_animated && src_tex != 0;
    binding->shader_src_frame = 0;
    binding->shader_hash = hash;

    GStatBuf st;
    binding->shader_mtime = (g_stat(path, &st) == 0) ? (int64_t)st.st_mtime : 0;
    binding->shader_check_us = INT64_MIN;
}

/*
 * Recompile the shader when its file changes on disk.
 *
 * The old program is kept until the new one links successfully, so a syntax
 * error while editing leaves the last working shader on screen (with the
 * error logged) instead of blanking the texture.
 *
 * The file is stat'd at most a few times a second: this runs from the
 * per-bind sweep, and stat'ing per draw call would be its own performance
 * problem.
 */
static void reload_texture_shader_if_changed(TextureBinding *binding,
                                             int64_t now_us)
{
    if (!binding->has_shader) {
        return;
    }

    if (binding->shader_check_us != INT64_MIN &&
        now_us >= binding->shader_check_us &&
        (now_us - binding->shader_check_us) < 250000) {
        return;
    }
    binding->shader_check_us = now_us;

    const char *path =
        nv2a_texture_io_get_shader_path(binding->shader_hash, NULL);
    if (path == NULL) {
        return;
    }

    GStatBuf st;
    if (g_stat(path, &st) != 0) {
        return;
    }

    int64_t mtime = (int64_t)st.st_mtime;
    if (mtime == binding->shader_mtime) {
        return;
    }
    binding->shader_mtime = mtime;

    GLuint program = compile_texture_shader(path);
    if (program == 0) {
        /* Compile failed; the previous program stays bound. */
        return;
    }

    glDeleteProgram(binding->shader_program);
    binding->shader_program = program;

    /* Force a redraw on the next refresh rather than waiting out the throttle. */
    binding->shader_last_us = INT64_MIN;

    fprintf(stderr, "nv2a: texture-io: reloaded shader %s\n", path);
}

/*
 * Refresh an animated iChannel0 before the shader samples it, so a .shader
 * paired with a .gif/.webp distorts live frames instead of a frozen one.
 */
static void refresh_shader_source(TextureBinding *binding, int64_t now_us)
{
    if (!binding->shader_src_animated || binding->shader_src == 0) {
        return;
    }

    int frame = nv2a_texture_io_animated_frame_index(binding->shader_hash,
                                                     NULL, now_us);
    if (frame < 0 || frame == binding->shader_src_frame) {
        return;
    }

    int sw = 0, sh = 0;
    const uint8_t *pixels = nv2a_texture_io_animated_frame_pixels(
        binding->shader_hash, NULL, frame, &sw, &sh);

    if (pixels == NULL) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, binding->shader_src);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    binding->shader_src_frame = frame;
}

/*
 * Render one frame of a binding's shader.
 *
 * Throttled to ~60Hz: bind_textures runs per draw call, so without this a
 * busy scene would re-render the shader thousands of times per frame.
 *
 * All GL state touched here is saved and restored. The nv2a renderer keeps
 * its own state assumptions across calls, and leaving a stray program, FBO,
 * or viewport bound would corrupt the frame in ways that are very hard to
 * trace back to here.
 */
static void render_texture_shader(TextureBinding *binding, int64_t now_us)
{
    if (!binding->has_shader) {
        return;
    }

    /* ~60Hz. INT64_MIN on a fresh binding forces the first render. */
    if (binding->shader_last_us != INT64_MIN &&
        now_us >= binding->shader_last_us &&
        (now_us - binding->shader_last_us) < 16000) {
        return;
    }
    binding->shader_last_us = now_us;

    /* Live iChannel0 before sampling it. */
    refresh_shader_source(binding, now_us);

    /* --- save state --- */
    GLint prev_fbo = 0, prev_program = 0, prev_vao = 0, prev_active = 0;
    GLint prev_viewport[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prev_blend = glIsEnabled(GL_BLEND);
    GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prev_stencil = glIsEnabled(GL_STENCIL_TEST);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);

    /* --- render --- */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, binding->shader_fbo);
    glViewport(0, 0, binding->shader_width, binding->shader_height);
    glUseProgram(binding->shader_program);

    glUniform1f(glGetUniformLocation(binding->shader_program, "iTime"),
                (float)(now_us / 1000) / 1000.0f);
    glUniform2f(glGetUniformLocation(binding->shader_program, "iResolution"),
                (float)binding->shader_width, (float)binding->shader_height);
    glUniform1i(glGetUniformLocation(binding->shader_program, "iFrame"),
                binding->shader_frame);
    glUniform1i(glGetUniformLocation(binding->shader_program, "iHasChannel0"),
                binding->shader_src != 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, binding->shader_src);
    glUniform1i(glGetUniformLocation(binding->shader_program, "iChannel0"), 0);

    /*
     * A VAO is required by core profile even when the vertex shader reads no
     * attributes. One is kept for the lifetime of the process rather than
     * created per draw.
     */
    static GLuint shader_vao;
    if (shader_vao == 0) {
        glGenVertexArrays(1, &shader_vao);
    }
    glBindVertexArray(shader_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    binding->shader_frame++;

    /* --- restore state --- */
    glBindVertexArray(prev_vao);
    glUseProgram(prev_program);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
               prev_viewport[3]);

    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (prev_blend) glEnable(GL_BLEND);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);

    glActiveTexture(prev_active);
    glBindTexture(GL_TEXTURE_2D, binding->gl_texture);
}

static void refresh_animated_binding(TextureBinding *binding, int64_t now_us)
{
    if (binding == NULL || !binding->is_animated) {
        return;
    }

    /*
     * Critical for performance: bind_textures runs once per draw call, not
     * once per frame, so a scene with thousands of draws would otherwise
     * re-upload and re-mip this texture thousands of times per frame. The
     * frame index is cheap to compute, so check it first and do nothing at
     * all unless the displayed frame actually changed -- for a 10fps GIF
     * that is ~10 uploads per second instead of thousands.
     */
    int frame = nv2a_texture_io_animated_frame_index(
        binding->anim_hash, binding->anim_variant, now_us);

    if (frame < 0 || frame == binding->anim_frame) {
        return;
    }

    glBindTexture(binding->gl_target, binding->gl_texture);

    /*
     * Sub-image update: all frames share dimensions, so there is no need to
     * reallocate storage with glTexImage2D.
     */
    int uploaded = binding->anim_frame;
    if (nv2a_texture_io_upload_animated_frame(
            binding->anim_hash, binding->anim_variant, binding->gl_target,
            /*full_upload=*/false, /*regen_mips=*/binding->anim_has_mips,
            now_us, &uploaded)) {
        binding->anim_frame = uploaded;
    }
}

/*
 * Advance every animated texture in the cache.
 *
 * PERFORMANCE: bind_textures() is called once per DRAW CALL, not once per
 * frame, and lru_add_free() puts free nodes on the global list too -- so an
 * unguarded walk here costs a full 512-node linked-list traversal on every
 * draw, whether or not any replacement texture exists. In a draw-heavy title
 * that is millions of cache-hostile pointer chases per frame, paid even with
 * texture replacement switched off. It looks like a game/renderer regression
 * rather than a feature cost, so it is gated twice:
 *
 *   1. Nothing can be animated unless replacements are enabled.
 *   2. Animation only needs to advance at the source media's frame rate
 *      (~10-30 fps for a typical GIF/WebP). 250 Hz is far more than enough
 *      and makes the cost per frame constant instead of per-draw.
 */
#define TEXTURE_ANIM_REFRESH_INTERVAL_US 4000

static void refresh_all_animated(PGRAPHGLState *r)
{
    if (!g_config.general.texture_replace_enabled) {
        return;
    }

    static int64_t last_refresh_us;
    int64_t now_us = nv2a_texture_io_anim_now_us();

    if (now_us >= last_refresh_us &&
        now_us - last_refresh_us < TEXTURE_ANIM_REFRESH_INTERVAL_US) {
        return;
    }
    last_refresh_us = now_us;

    LruNode *node;
    QTAILQ_FOREACH(node, &r->texture_cache.global, next_global) {
        TextureLruNode *tnode = container_of(node, TextureLruNode, node);
        if (tnode->binding == NULL) {
            continue;
        }
        if (tnode->binding->is_animated) {
            refresh_animated_binding(tnode->binding, now_us);
        }
        if (tnode->binding->has_shader) {
            reload_texture_shader_if_changed(tnode->binding, now_us);
            render_texture_shader(tnode->binding, now_us);
        }
    }
}

void pgraph_gl_bind_textures(NV2AState *d)
{
    if (nv2a_texture_io_consume_flush_request()) {
        pgraph_gl_texture_cache_flush();
    }

    int i;
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_GL_DGROUP_BEGIN("%s", __func__);

    /* Advance animated replacements before any stage is bound below. */
    refresh_all_animated(r);

    for (i=0; i<NV2A_MAX_TEXTURES; i++) {
        bool enabled = pgraph_is_texture_enabled(pg, i);
        /* FIXME: What happens if texture is disabled but stage is active? */

        glActiveTexture(GL_TEXTURE0 + i);
        if (!enabled) {
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            glBindTexture(GL_TEXTURE_1D, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_3D, 0);
            continue;
        }

        uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i*4);
        uint32_t address = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i*4);
        uint32_t border_color = pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + i*4);
        uint32_t max_anisotropy =
            1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i*4),
                           NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

        /* Check for unsupported features */
        if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED) NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
        if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED) NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
        if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED) NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
        if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED) NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

        TextureShape state = pgraph_get_texture_shape(pg, i);
        hwaddr texture_vram_offset, palette_vram_offset;
        size_t length, palette_length;

        length = pgraph_get_texture_length(pg, &state);
        texture_vram_offset = pgraph_get_texture_phys_addr(pg, i);
        palette_vram_offset = pgraph_get_texture_palette_phys_addr_length(pg, i, &palette_length);

        assert((texture_vram_offset + length) < memory_region_size(d->vram));
        assert((palette_vram_offset + palette_length)
               < memory_region_size(d->vram));
        bool is_indexed = (state.color_format ==
                NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
        bool possibly_dirty = false;
        bool possibly_dirty_checked = false;

        SurfaceBinding *surface = pgraph_gl_surface_get(d, texture_vram_offset);
        TextureBinding *tbind = r->texture_binding[i];
        if (!pg->texture_dirty[i] && tbind) {
            bool reusable = false;
            if (surface && tbind->draw_time == surface->draw_time) {
                reusable = true;
            } else if (!surface) {
                possibly_dirty = check_texture_possibly_dirty(
                        d,
                        texture_vram_offset,
                        length,
                        palette_vram_offset,
                        is_indexed ? palette_length : 0);
                possibly_dirty_checked = true;
                reusable = !possibly_dirty;
            }

            if (reusable) {
                glBindTexture(r->texture_binding[i]->gl_target,
                              r->texture_binding[i]->gl_texture);
                /*
                 * Only read the clock when there is something animated to
                 * advance. This runs per draw call per texture stage, so an
                 * unconditional qemu_clock_get_us() here is hundreds of
                 * thousands of clock reads per second in a busy scene.
                 */
                if (r->texture_binding[i]->is_animated) {
                    refresh_animated_binding(r->texture_binding[i],
                                             nv2a_texture_io_anim_now_us());
                }
                apply_texture_parameters(r,
                                         r->texture_binding[i],
                                         &kelvin_color_format_info_map[state.color_format],
                                         state.dimensionality,
                                         filter,
                                         address,
                                         state.border,
                                         border_color,
                                         max_anisotropy);
                continue;
            }
        }

        /*
         * Check active surfaces to see if this texture was a render target
         */
        bool surf_to_tex = false;
        if (surface != NULL) {
            surf_to_tex = pgraph_gl_check_surface_to_texture_compatibility(
                    surface, &state);

            if (surf_to_tex && surface->upload_pending) {
                pgraph_gl_upload_surface_data(d, surface, false);
            }
        }

        if (!surf_to_tex) {
            // FIXME: Restructure to support rendering surfaces to cubemap faces

            // Writeback any surfaces which this texture may index
            hwaddr tex_vram_end = texture_vram_offset + length - 1;
            QTAILQ_FOREACH(surface, &r->surfaces, entry) {
                hwaddr surf_vram_end = surface->vram_addr + surface->size - 1;
                bool overlapping = !(surface->vram_addr >= tex_vram_end
                                     || texture_vram_offset >= surf_vram_end);
                if (overlapping) {
                    pgraph_gl_surface_download_if_dirty(d, surface);
                }
            }
        }

        TextureKey key;
        memset(&key, 0, sizeof(TextureKey));
        key.state = state;
        key.texture_vram_offset = texture_vram_offset;
        key.texture_length = length;
        if (is_indexed) {
            key.palette_vram_offset = palette_vram_offset;
            key.palette_length = palette_length;
        }

        // Search for existing texture binding in cache
        uint64_t tex_binding_hash = fast_hash((uint8_t*)&key, sizeof(key));
        LruNode *found = lru_lookup(&r->texture_cache,
                                     tex_binding_hash, &key);
        TextureLruNode *key_out = container_of(found, TextureLruNode, node);
        possibly_dirty |= (key_out->binding == NULL) || key_out->possibly_dirty;

        if (!surf_to_tex && !possibly_dirty_checked) {
            possibly_dirty |= check_texture_possibly_dirty(
                    d,
                    texture_vram_offset,
                    length,
                    palette_vram_offset,
                    is_indexed ? palette_length : 0);
        }

        // Calculate hash of texture data, if necessary
        void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
        void *palette_data = (char*)d->vram_ptr + palette_vram_offset;

        uint64_t tex_data_hash = 0;
        if (!surf_to_tex && possibly_dirty) {
            tex_data_hash = fast_hash(texture_data, length);
            if (is_indexed) {
                tex_data_hash ^= fast_hash(palette_data, palette_length);
            }
        }

        // Free existing binding, if texture data has changed
        bool must_destroy = (key_out->binding != NULL)
                            && possibly_dirty
                            && (key_out->binding->data_hash != tex_data_hash);
        if (must_destroy) {
            texture_binding_destroy(key_out->binding);
            key_out->binding = NULL;
        }

        if (key_out->binding == NULL) {
            // Must create the texture
            key_out->binding = generate_texture(state, texture_data, palette_data,
                                               tex_data_hash);
            key_out->binding->data_hash = tex_data_hash;
            key_out->binding->scale = 1;
        } else {
            // Saved an upload! Reuse existing texture in graphics memory.
            glBindTexture(key_out->binding->gl_target,
                          key_out->binding->gl_texture);
            if (key_out->binding->is_animated) {
                refresh_animated_binding(key_out->binding,
                                         nv2a_texture_io_anim_now_us());
            }
        }

        key_out->possibly_dirty = false;
        TextureBinding *binding = key_out->binding;
        binding->refcnt++;

        if (surf_to_tex && binding->draw_time < surface->draw_time) {

            trace_nv2a_pgraph_surface_render_to_texture(
                surface->vram_addr, surface->width, surface->height);
            pgraph_gl_render_surface_to_texture(d, surface, binding, &state, i);
            binding->draw_time = surface->draw_time;
            binding->scale = pg->surface_scale_factor;
        }

        apply_texture_parameters(r,
                                 binding,
                                 &kelvin_color_format_info_map[state.color_format],
                                 state.dimensionality,
                                 filter,
                                 address,
                                 state.border,
                                 border_color,
                                 max_anisotropy);

        if (r->texture_binding[i]) {
            if (r->texture_binding[i]->gl_target != binding->gl_target) {
                glBindTexture(r->texture_binding[i]->gl_target, 0);
            }
            texture_binding_destroy(r->texture_binding[i]);
        }
        r->texture_binding[i] = binding;
        pg->texture_dirty[i] = false;
    }
    NV2A_GL_DGROUP_END();
}

static enum S3TC_DECOMPRESS_FORMAT
gl_internal_format_to_s3tc_enum(GLint gl_internal_format)
{
    switch (gl_internal_format) {
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(!"Invalid gl_internal_format");
    }
}

/* Cube map face index for a GL target, or -1 for non-cubemap targets. */
static int gl_target_face_index(GLenum gl_target)
{
    switch (gl_target) {
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X: return 0;
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X: return 1;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: return 2;
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y: return 3;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: return 4;
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: return 5;
    default: return -1;
    }
}

/*
 * Build the dump variant for a given face/level combination:
 *   plain 2D level 0  -> NULL
 *   cube face 2 lvl 0 -> "posy"
 *   plain 2D level 3  -> "mip3"
 *   cube face 2 lvl 3 -> "posy_mip3"
 */
static void build_dump_variant(char *out, size_t out_size, int face, int level)
{
    const char *face_name =
        face >= 0 ? nv2a_texture_io_cubemap_face_name(face) : NULL;

    if (face_name && level > 0) {
        snprintf(out, out_size, "%s_mip%d", face_name, level);
    } else if (face_name) {
        snprintf(out, out_size, "%s", face_name);
    } else if (level > 0) {
        snprintf(out, out_size, "mip%d", level);
    } else {
        out[0] = '\0';
    }
}

static void upload_gl_texture(GLenum gl_target,
                              const TextureShape s,
                              const uint8_t *texture_data,
                              const uint8_t *palette_data,
                              uint64_t data_hash)
{
    const int dump_face = gl_target_face_index(gl_target);
    char dump_variant[32];
    ColorFormatInfo f = kelvin_color_format_gl_map[s.color_format];
    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    unsigned int adjusted_width = s.width;
    unsigned int adjusted_height = s.height;
    unsigned int adjusted_pitch = s.pitch;
    unsigned int adjusted_depth = s.depth;
    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    switch(gl_target) {
    case GL_TEXTURE_1D:
        assert(!"Invalid 1D gl target texture");
        break;
    case GL_TEXTURE_2D:
        if (f.linear) {
            /* Can't handle strides unaligned to pixels */
            assert(s.pitch % f.bytes_per_pixel == 0);

            uint8_t *converted = pgraph_convert_texture_data(
                s, texture_data, palette_data, adjusted_width, adjusted_height, 1,
                adjusted_pitch, 0, NULL);
            glPixelStorei(GL_UNPACK_ROW_LENGTH,
                          converted ? 0 : adjusted_pitch / f.bytes_per_pixel);
            glTexImage2D(GL_TEXTURE_2D, 0, f.gl_internal_format,
                         adjusted_width, adjusted_height, 0,
                         f.gl_format, f.gl_type,
                         converted ? converted : texture_data);

            if (data_hash != 0 && g_config.general.texture_dump_enabled &&
                f.bytes_per_pixel == 4) {
                nv2a_texture_io_dump(data_hash, adjusted_width,
                                     adjusted_height,
                                     converted ? converted : texture_data);
            }

            if (converted) {
              g_free(converted);
            }

            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            break;
        }
        /* fallthru */
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: {

        unsigned int width = adjusted_width, height = adjusted_height;

        int level;
        for (level = 0; level < s.levels; level++) {
            width = MAX(width, 1);
            height = MAX(height, 1);

            if (f.gl_format == 0) { /* compressed */
                 // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                unsigned int block_size =
                    f.gl_internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ?
                        8 : 16;
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                uint8_t *converted = s3tc_decompress_2d(
                    gl_internal_format_to_s3tc_enum(f.gl_internal_format),
                    texture_data, width, height);
                unsigned int tex_width = width;
                unsigned int tex_height = height;

                if (s.cubemap && adjusted_width != s.width) {
                    // FIXME: Consider preserving the border.
                    // There does not seem to be a way to reference the border
                    // texels in a cubemap, so they are discarded.
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
                    tex_width = s.width;
                    tex_height = s.height;
                    if (physical_width == width) {
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                    }
                }

                glTexImage2D(gl_target, level, GL_RGBA, tex_width, tex_height, 0,
                             GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, converted);
                if (data_hash != 0 && g_config.general.texture_dump_enabled &&
                    (level == 0 || g_config.general.texture_dump_mipmaps)) {
                    build_dump_variant(dump_variant, sizeof(dump_variant),
                                       dump_face, level);
                    nv2a_texture_io_dump_variant(
                        data_hash, dump_variant[0] ? dump_variant : NULL,
                        tex_width, tex_height, converted);
                }
                g_free(converted);
                if (s.cubemap && adjusted_width != s.width) {
                    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                    if (physical_width == width) {
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    }
                }
                texture_data +=
                    physical_width / 4 * physical_height / 4 * block_size;
            } else {
                unsigned int pitch = width * f.bytes_per_pixel;
                uint8_t *unswizzled = (uint8_t*)g_malloc(height * pitch);
                unswizzle_rect(texture_data, width, height,
                               unswizzled, pitch, f.bytes_per_pixel);
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data, width, height, 1, pitch, 0,
                    NULL);
                uint8_t *pixel_data = converted ? converted : unswizzled;
                unsigned int tex_width = width;
                unsigned int tex_height = height;

                if (s.cubemap && adjusted_width != s.width) {
                    // FIXME: Consider preserving the border.
                    // There does not seem to be a way to reference the border
                    // texels in a cubemap, so they are discarded.
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                    tex_width = s.width;
                    tex_height = s.height;
                    pixel_data += 4 * f.bytes_per_pixel + 4 * pitch;
                }

                glTexImage2D(gl_target, level, f.gl_internal_format, tex_width,
                             tex_height, 0, f.gl_format, f.gl_type,
                             pixel_data);
                if (data_hash != 0 && g_config.general.texture_dump_enabled &&
                    f.bytes_per_pixel == 4 &&
                    (level == 0 || g_config.general.texture_dump_mipmaps)) {
                    build_dump_variant(dump_variant, sizeof(dump_variant),
                                       dump_face, level);
                    nv2a_texture_io_dump_variant(
                        data_hash, dump_variant[0] ? dump_variant : NULL,
                        tex_width, tex_height, pixel_data);
                }
                if (s.cubemap && s.border) {
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                }
                if (converted) {
                    g_free(converted);
                }
                g_free(unswizzled);

                texture_data += width * height * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
        }

        break;
    }
    case GL_TEXTURE_3D: {

        unsigned int width = adjusted_width;
        unsigned int height = adjusted_height;
        unsigned int depth = adjusted_depth;

        assert(f.linear == false);

        int level;
        for (level = 0; level < s.levels; level++) {
            if (f.gl_format == 0) { /* compressed */
                width = MAX(width, 1);
                height = MAX(height, 1);
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                depth = MAX(depth, 1);

                unsigned int block_size;
                if (f.gl_internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
                    block_size = 8;
                } else {
                    block_size = 16;
                }

                size_t texture_size = physical_width/4 * physical_height/4 * depth * block_size;

                uint8_t *converted = s3tc_decompress_3d(
                    gl_internal_format_to_s3tc_enum(f.gl_internal_format),
                    texture_data, width, height, depth);

                glTexImage3D(gl_target, level,  GL_RGBA8,
                             width, height, depth, 0,
                             GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV,
                             converted);

                g_free(converted);

                texture_data += texture_size;
            } else {
                width = MAX(width, 1);
                height = MAX(height, 1);
                depth = MAX(depth, 1);

                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;
                uint8_t *unswizzled = (uint8_t*)g_malloc(slice_pitch * depth);
                unswizzle_box(texture_data, width, height, depth, unswizzled,
                               row_pitch, slice_pitch, f.bytes_per_pixel);

                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data, width, height, depth,
                    row_pitch, slice_pitch, NULL);

                glTexImage3D(gl_target, level, f.gl_internal_format,
                             width, height, depth, 0,
                             f.gl_format, f.gl_type,
                             converted ? converted : unswizzled);

                if (converted) {
                    g_free(converted);
                }
                g_free(unswizzled);

                texture_data += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
        break;
    }
    default:
        assert(!"Invalid gl target texture type");
        break;
    }
}

static TextureBinding* generate_texture(const TextureShape s,
                                        const uint8_t *texture_data,
                                        const uint8_t *palette_data,
                                        uint64_t data_hash)
{
    ColorFormatInfo f = kelvin_color_format_gl_map[s.color_format];

    /* Create a new opengl texture */
    GLuint gl_texture;
    glGenTextures(1, &gl_texture);

    GLenum gl_target;
    if (s.cubemap) {
        assert(f.linear == false);
        assert(s.dimensionality == 2);
        gl_target = GL_TEXTURE_CUBE_MAP;
    } else {
        if (f.linear) {
            gl_target = GL_TEXTURE_2D;
            assert(s.dimensionality == 2);
        } else {
            switch(s.dimensionality) {
            case 1: gl_target = GL_TEXTURE_1D; break;
            case 2: gl_target = GL_TEXTURE_2D; break;
            case 3: gl_target = GL_TEXTURE_3D; break;
            default:
                NV2A_GL_DPRINTF(true, "Invalid texture dimensionality: %u", (unsigned)s.dimensionality);
                assert(!"Invalid texture dimensionality");
                break;
            }
        }
    }

    glBindTexture(gl_target, gl_texture);

    /*
     * Texture replacement: if a pack supplies an image for this data hash,
     * upload it in place of the guest texture. Cubemaps are not currently
     * supported (each face would need its own replacement file), and
     * surface-sourced textures never reach here with a valid hash.
     */
    bool replaced = false;
    if (g_config.general.texture_replace_enabled && data_hash != 0 &&
        gl_target != GL_TEXTURE_3D) {
        if (gl_target == GL_TEXTURE_CUBE_MAP) {
            /*
             * All six faces must be present and the same size, otherwise the
             * guest cubemap is used unmodified -- a half-replaced cube map
             * would look far worse than none at all.
             */
            int cw = 0, ch = 0;
            if (nv2a_texture_io_has_all_cubemap_faces(data_hash, &cw, &ch)) {
                static const GLenum face_targets[6] = {
                    GL_TEXTURE_CUBE_MAP_POSITIVE_X,
                    GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
                    GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
                    GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
                    GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
                    GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
                };

                replaced = true;
                for (int face = 0; face < 6; face++) {
                    if (!nv2a_texture_io_try_upload_replacement_target(
                            data_hash,
                            nv2a_texture_io_cubemap_face_name(face),
                            face_targets[face], false)) {
                        replaced = false;
                        break;
                    }
                }

                /* Mip chain is generated once, after all faces are present. */
                if (replaced && s.levels > 1) {
                    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
                }
            }
        } else {
            replaced = nv2a_texture_io_try_upload_replacement(data_hash,
                                                              s.levels > 1);
        }
    }

    bool is_animated = replaced && gl_target != GL_TEXTURE_CUBE_MAP &&
                       nv2a_texture_io_replacement_is_animated(data_hash,
                                                               NULL);

    NV2A_GL_DLABEL(GL_TEXTURE, gl_texture,
                   "offset: 0x%08lx, format: 0x%02X%s, %d dimensions%s, "
                   "width: %d, height: %d, depth: %d",
                   texture_data - g_nv2a->vram_ptr,
                   s.color_format, f.linear ? "" : " (SZ)",
                   s.dimensionality, s.cubemap ? " (Cubemap)" : "",
                   s.width, s.height, s.depth);

    if (replaced) {
        /* Replacement image already uploaded; skip guest texture upload. */
    } else if (gl_target == GL_TEXTURE_CUBE_MAP) {
        unsigned int block_size;
        if (f.gl_internal_format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
            block_size = 8;
        } else {
            block_size = 16;
        }

        size_t length = 0;
        unsigned int w = s.width;
        unsigned int h = s.height;
        if (!f.linear && s.border) {
            w = MAX(16, w * 2);
            h = MAX(16, h * 2);
        }

        int level;
        for (level = 0; level < s.levels; level++) {
            if (f.gl_format == 0) {
                length += w/4 * h/4 * block_size;
            } else {
                length += w * h * f.bytes_per_pixel;
            }

            w /= 2;
            h /= 2;
        }

        length = (length + NV2A_CUBEMAP_FACE_ALIGNMENT - 1) & ~(NV2A_CUBEMAP_FACE_ALIGNMENT - 1);

        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_X,
                          s, texture_data + 0 * length, palette_data, data_hash);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
                          s, texture_data + 1 * length, palette_data, data_hash);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
                          s, texture_data + 2 * length, palette_data, data_hash);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
                          s, texture_data + 3 * length, palette_data, data_hash);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
                          s, texture_data + 4 * length, palette_data, data_hash);
        upload_gl_texture(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
                          s, texture_data + 5 * length, palette_data, data_hash);
    } else {
        upload_gl_texture(gl_target, s, texture_data, palette_data, data_hash);
    }

    /* Linear textures don't support mipmapping */
    if (!f.linear && !replaced) {
        glTexParameteri(gl_target, GL_TEXTURE_BASE_LEVEL,
            s.min_mipmap_level);
        glTexParameteri(gl_target, GL_TEXTURE_MAX_LEVEL,
            s.levels - 1);
    } else if (replaced) {
        /*
         * Replacement dimensions do not match the guest mip chain, so the
         * guest level bounds cannot be applied.
         *
         * MAX_LEVEL must still be set. Its OpenGL default is 1000, so if the
         * replacement uploaded only level 0 (which is what happens whenever
         * the guest texture had no mip chain, since gen_mipmaps is passed as
         * s.levels > 1) the texture is mipmap-INCOMPLETE for any mipmapping
         * min filter -- and OpenGL samples an incomplete texture as BLACK.
         * That is the cause of replacements appearing as black boxes on the
         * GL backend while working on Vulkan, which has no equivalent
         * completeness rule.
         *
         * When glGenerateMipmap did run it produced a full chain down to
         * 1x1, so leaving MAX_LEVEL high is correct in that case.
         */
        glTexParameteri(gl_target, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(gl_target, GL_TEXTURE_MAX_LEVEL,
                        s.levels > 1 ? 1000 : 0);
    }

    if (!replaced &&
        (f.gl_swizzle_mask[0] != 0 || f.gl_swizzle_mask[1] != 0
        || f.gl_swizzle_mask[2] != 0 || f.gl_swizzle_mask[3] != 0)) {
        glTexParameteriv(gl_target, GL_TEXTURE_SWIZZLE_RGBA,
                         (const GLint *)f.gl_swizzle_mask);
    }

    TextureBinding* ret = (TextureBinding *)g_malloc(sizeof(TextureBinding));
    ret->gl_target = gl_target;
    ret->gl_texture = gl_texture;
    ret->refcnt = 1;
    ret->draw_time = 0;
    ret->data_hash = 0;
    ret->min_filter = 0xFFFFFFFF;
    ret->mag_filter = 0xFFFFFFFF;
    ret->lod_bias = 0xFFFFFFFF;
    ret->addru = 0xFFFFFFFF;
    ret->addrv = 0xFFFFFFFF;
    ret->addrp = 0xFFFFFFFF;
    ret->border_color_set = false;
    ret->is_animated = is_animated;
    ret->anim_hash = is_animated ? data_hash : 0;
    ret->anim_variant = NULL; /* only plain textures are animated for now */
    ret->anim_frame = 0;
    ret->anim_has_mips = is_animated && s.levels > 1;

    ret->has_shader = false;
    ret->shader_program = 0;
    ret->shader_fbo = 0;
    ret->shader_src = 0;
    ret->shader_width = 0;
    ret->shader_height = 0;
    ret->shader_frame = 0;
    ret->shader_last_us = INT64_MIN;
    ret->shader_src_animated = false;
    ret->shader_src_frame = 0;
    ret->shader_hash = 0;
    ret->shader_mtime = 0;
    ret->shader_check_us = INT64_MIN;

    /*
     * Procedural shader attachment. Set up after the binding exists because
     * it reallocates gl_texture as its render target; a shader therefore
     * supersedes whatever was uploaded above, using an image replacement (if
     * any) as its iChannel0 source rather than as the final texture.
     *
     * 2D only: a cubemap would need a target per face.
     */
    if (g_config.general.texture_replace_enabled && data_hash != 0 &&
        gl_target == GL_TEXTURE_2D) {
        setup_texture_shader(ret, data_hash, s.width, s.height);

        /*
         * Render once immediately, as the Vulkan path does. setup_ only
         * allocates the render target; until something draws into it the
         * texture holds undefined contents, which shows as black. The
         * animated sweep runs at the TOP of bind_textures, so a binding
         * created here is not swept until a later call.
         */
        if (ret->has_shader) {
            render_texture_shader(ret, nv2a_texture_io_anim_now_us());
        }
    }

    return ret;
}

static void texture_binding_destroy(gpointer data)
{
    TextureBinding *binding = (TextureBinding *)data;
    assert(binding->refcnt > 0);
    binding->refcnt--;
    if (binding->refcnt == 0) {
        g_free(binding->anim_variant);
        if (binding->shader_fbo) {
            glDeleteFramebuffers(1, &binding->shader_fbo);
        }
        if (binding->shader_program) {
            glDeleteProgram(binding->shader_program);
        }
        if (binding->shader_src) {
            glDeleteTextures(1, &binding->shader_src);
        }
        glDeleteTextures(1, &binding->gl_texture);
        g_free(binding);
    }
}

/* functions for texture LRU cache */
static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *key)
{
    TextureLruNode *tnode = container_of(node, TextureLruNode, node);
    memcpy(&tnode->key, key, sizeof(TextureKey));

    tnode->binding = NULL;
    tnode->possibly_dirty = false;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    TextureLruNode *tnode = container_of(node, TextureLruNode, node);
    if (tnode->binding) {
        texture_binding_destroy(tnode->binding);
        tnode->binding = NULL;
        tnode->possibly_dirty = false;
    }
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureLruNode *tnode = container_of(node, TextureLruNode, node);
    return memcmp(&tnode->key, key, sizeof(TextureKey));
}

void pgraph_gl_init_textures(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    const size_t texture_cache_size = 512;
    lru_init(&r->texture_cache);
    /*
     * calloc rather than malloc: the animated-texture sweep walks the
     * LRU global list, which includes not-yet-used free nodes. Those
     * must have binding == NULL rather than uninitialised garbage.
     */
    r->texture_cache_entries = calloc(texture_cache_size, sizeof(TextureLruNode));
    assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }

    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;

    nv2a_texture_io_set_backend(NV2A_TEXTURE_BACKEND_GL);
}

void pgraph_gl_finalize_textures(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_binding[i] = NULL;
    }

    lru_flush(&r->texture_cache);
    free(r->texture_cache_entries);

    r->texture_cache_entries = NULL;
}

void pgraph_gl_texture_cache_flush(void)
{
    /*
     * gl_renderer_state and vk_renderer_state alias the same union member,
     * so a NULL check alone is not sufficient -- under Vulkan the pointer
     * is non-NULL but points at PGRAPHVkState.
     */
    if (g_nv2a == NULL ||
        nv2a_texture_io_get_backend() != NV2A_TEXTURE_BACKEND_GL) {
        return;
    }

    PGRAPHState *pg = &g_nv2a->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (r == NULL || r->texture_cache_entries == NULL) {
        return;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_binding[i] = NULL;
    }

    lru_flush(&r->texture_cache);
}

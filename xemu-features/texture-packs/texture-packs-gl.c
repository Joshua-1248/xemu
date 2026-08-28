/*
 * OpenGL adapter for the isolated texture-pack feature.
 */
#include "qemu/osdep.h"
#include <epoxy/gl.h>

#include "xemu-features/texture-packs/texture-packs.h"
#include "xemu-features/texture-packs/texture-packs-gl.h"

#include <glib/gstdio.h>
#include "hw/xbox/nv2a/pgraph/gl/renderer.h"

typedef struct XemuTexturePacksGLBindingState {
    TextureBinding *binding;
    bool is_animated;
    uint64_t anim_hash;
    char *anim_variant;
    int anim_frame;
    bool anim_has_mips;

    bool has_shader;
    GLuint shader_program;
    GLuint shader_fbo;
    GLuint shader_src;
    GLint shader_u_time;
    GLint shader_u_resolution;
    GLint shader_u_frame;
    GLint shader_u_has_channel0;
    GLint shader_u_channel0;
    int shader_width;
    int shader_height;
    int shader_frame;
    int64_t shader_last_us;
    bool shader_src_animated;
    int shader_src_frame;
    uint64_t shader_hash;
    int64_t shader_mtime;
    int64_t shader_check_us;
} XemuTexturePacksGLBindingState;

static GHashTable *gl_binding_states;

static XemuTexturePacksGLBindingState *gl_binding_state_lookup(TextureBinding *binding)
{
    return gl_binding_states != NULL ?
        g_hash_table_lookup(gl_binding_states, binding) : NULL;
}

static XemuTexturePacksGLBindingState *gl_binding_state_create(TextureBinding *binding)
{
    if (gl_binding_states == NULL) {
        gl_binding_states = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    XemuTexturePacksGLBindingState *state = g_new0(XemuTexturePacksGLBindingState, 1);
    state->binding = binding;
    state->anim_frame = 0;
    state->shader_u_time = -1;
    state->shader_u_resolution = -1;
    state->shader_u_frame = -1;
    state->shader_u_has_channel0 = -1;
    state->shader_u_channel0 = -1;
    state->shader_last_us = INT64_MIN;
    state->shader_check_us = INT64_MIN;
    g_hash_table_insert(gl_binding_states, binding, state);
    return state;
}


bool xemu_texture_packs_gl_upload_animated_frame(
    uint64_t hash, const char *variant, unsigned int gl_target,
    bool full_upload, bool regen_mips, int64_t now_us, int *out_frame)
{
    int frame = xemu_texture_packs_animated_frame_index(hash, variant, now_us);
    if (frame < 0) {
        return false;
    }

    int width = 0;
    int height = 0;
    const uint8_t *pixels = xemu_texture_packs_animated_frame_pixels(
        hash, variant, frame, &width, &height);
    if (pixels == NULL || width <= 0 || height <= 0) {
        return false;
    }

    if (out_frame != NULL) {
        *out_frame = frame;
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (full_upload) {
        glTexImage2D((GLenum)gl_target, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    } else {
        glTexSubImage2D((GLenum)gl_target, 0, 0, 0, width, height,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    if (regen_mips) {
        glGenerateMipmap(gl_target == GL_TEXTURE_2D ? GL_TEXTURE_2D :
                                                       GL_TEXTURE_CUBE_MAP);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return true;
}

bool xemu_texture_packs_gl_try_upload_replacement_target(
    uint64_t hash, const char *variant, unsigned int gl_target,
    bool gen_mipmaps)
{
    int frame = -1;
    if (xemu_texture_packs_gl_upload_animated_frame(
            hash, variant, gl_target, true, gen_mipmaps,
            xemu_texture_packs_anim_now_us(), &frame)) {
        return true;
    }

    int width = 0;
    int height = 0;
    uint8_t *pixels = xemu_texture_packs_load_replacement_rgba_variant(
        hash, variant, &width, &height);
    if (pixels == NULL) {
        return false;
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D((GLenum)gl_target, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (gen_mipmaps) {
        glGenerateMipmap(gl_target == GL_TEXTURE_2D ? GL_TEXTURE_2D :
                                                       GL_TEXTURE_CUBE_MAP);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    xemu_texture_packs_free_pixels(pixels);
    return true;
}

bool xemu_texture_packs_gl_try_upload_replacement(uint64_t hash,
                                                   bool gen_mipmaps)
{
    return xemu_texture_packs_gl_try_upload_replacement_target(
        hash, NULL, GL_TEXTURE_2D, gen_mipmaps);
}


static int gl_target_face_index(unsigned int gl_target)
{
    switch ((GLenum)gl_target) {
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X: return 0;
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X: return 1;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: return 2;
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y: return 3;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: return 4;
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: return 5;
    default: return -1;
    }
}

static void build_dump_variant(char *out, size_t out_size, int face, int level)
{
    const char *face_name =
        face >= 0 ? xemu_texture_packs_cubemap_face_name(face) : NULL;

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

void xemu_texture_packs_gl_maybe_dump_guest32(
    uint64_t hash, unsigned int gl_target, int level,
    unsigned int width, unsigned int height, unsigned int row_stride,
    uint32_t guest_color_format, const uint8_t *data)
{
    if (hash == 0 || data == NULL ||
        !xemu_texture_packs_should_dump_level(level)) {
        return;
    }

    char variant[32];
    build_dump_variant(variant, sizeof(variant),
                       gl_target_face_index(gl_target), level);
    xemu_texture_packs_dump_guest32_variant(
        hash, variant[0] ? variant : NULL, width, height, row_stride,
        guest_color_format, data);
}

bool xemu_texture_packs_gl_try_replace_bound_texture(
    uint64_t hash, unsigned int gl_target, int guest_levels,
    bool *out_animated)
{
    if (out_animated != NULL) {
        *out_animated = false;
    }
    if (!xemu_texture_packs_replace_enabled() || hash == 0 ||
        gl_target == GL_TEXTURE_3D) {
        return false;
    }

    bool replaced = false;
    if (gl_target == GL_TEXTURE_CUBE_MAP) {
        int cw = 0, ch = 0;
        if (!xemu_texture_packs_has_all_cubemap_faces(hash, &cw, &ch)) {
            return false;
        }
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
            if (!xemu_texture_packs_gl_try_upload_replacement_target(
                    hash, xemu_texture_packs_cubemap_face_name(face),
                    face_targets[face], false)) {
                replaced = false;
                break;
            }
        }
        if (replaced && guest_levels > 1) {
            glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        }
    } else {
        replaced = xemu_texture_packs_gl_try_upload_replacement(
            hash, guest_levels > 1);
    }

    if (replaced && out_animated != NULL &&
        gl_target != GL_TEXTURE_CUBE_MAP) {
        *out_animated = xemu_texture_packs_replacement_is_animated(hash, NULL);
    }
    return replaced;
}

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

static void cache_texture_shader_uniforms(XemuTexturePacksGLBindingState *state)
{
    state->shader_u_time =
        glGetUniformLocation(state->shader_program, "iTime");
    state->shader_u_resolution =
        glGetUniformLocation(state->shader_program, "iResolution");
    state->shader_u_frame =
        glGetUniformLocation(state->shader_program, "iFrame");
    state->shader_u_has_channel0 =
        glGetUniformLocation(state->shader_program, "iHasChannel0");
    state->shader_u_channel0 =
        glGetUniformLocation(state->shader_program, "iChannel0");
}

/*
 * Set up a binding's shader: compile it, size the render target, capture any
 * image replacement as iChannel0, and build the FBO. Called once, at binding
 * creation. Leaves has_shader false on any failure.
 */
static void setup_texture_shader(XemuTexturePacksGLBindingState *state, uint64_t hash,
                                 int guest_width, int guest_height)
{
    const char *path = xemu_texture_packs_get_shader_path(hash, NULL);
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
    bool have_source = xemu_texture_packs_get_replacement_size(hash, &rw, &rh);

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
    bool src_animated = xemu_texture_packs_replacement_is_animated(hash, NULL);

    if (have_source) {
        int sw = 0, sh = 0;
        uint8_t *pixels = NULL;
        const uint8_t *frame_pixels = NULL;

        if (src_animated) {
            /*
             * Borrowed from the decoded frame cache -- must not be freed,
             * and covers WebP, which stbi_load cannot read at all.
             */
            frame_pixels = xemu_texture_packs_animated_frame_pixels(
                hash, NULL, 0, &sw, &sh);
        } else {
            pixels = xemu_texture_packs_load_replacement_rgba(hash, &sw, &sh);
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
            xemu_texture_packs_free_pixels(pixels);
        }
    }

    /* Allocate the render target at the chosen size. */
    glBindTexture(GL_TEXTURE_2D, state->binding->gl_texture);
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
                           GL_TEXTURE_2D, state->binding->gl_texture, 0);

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

    state->has_shader = true;
    state->shader_program = program;
    cache_texture_shader_uniforms(state);
    state->shader_fbo = fbo;
    state->shader_src = src_tex;
    state->shader_width = w;
    state->shader_height = h;
    state->shader_frame = 0;
    state->shader_last_us = INT64_MIN;
    state->shader_src_animated = src_animated && src_tex != 0;
    state->shader_src_frame = 0;
    state->shader_hash = hash;

    GStatBuf st;
    state->shader_mtime = (g_stat(path, &st) == 0) ? (int64_t)st.st_mtime : 0;
    state->shader_check_us = INT64_MIN;
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
static void reload_texture_shader_if_changed(XemuTexturePacksGLBindingState *state,
                                             int64_t now_us)
{
    if (!state->has_shader) {
        return;
    }

    if (state->shader_check_us != INT64_MIN &&
        now_us >= state->shader_check_us &&
        (now_us - state->shader_check_us) < 250000) {
        return;
    }
    state->shader_check_us = now_us;

    const char *path =
        xemu_texture_packs_get_shader_path(state->shader_hash, NULL);
    if (path == NULL) {
        return;
    }

    GStatBuf st;
    if (g_stat(path, &st) != 0) {
        return;
    }

    int64_t mtime = (int64_t)st.st_mtime;
    if (mtime == state->shader_mtime) {
        return;
    }
    state->shader_mtime = mtime;

    GLuint program = compile_texture_shader(path);
    if (program == 0) {
        /* Compile failed; the previous program stays bound. */
        return;
    }

    glDeleteProgram(state->shader_program);
    state->shader_program = program;
    cache_texture_shader_uniforms(state);

    /* Force a redraw on the next refresh rather than waiting out the throttle. */
    state->shader_last_us = INT64_MIN;

    fprintf(stderr, "nv2a: texture-io: reloaded shader %s\n", path);
}

/*
 * Refresh an animated iChannel0 before the shader samples it, so a .shader
 * paired with a .gif/.webp distorts live frames instead of a frozen one.
 */
static void refresh_shader_source(XemuTexturePacksGLBindingState *state, int64_t now_us)
{
    if (!state->shader_src_animated || state->shader_src == 0) {
        return;
    }

    int frame = xemu_texture_packs_animated_frame_index(state->shader_hash,
                                                     NULL, now_us);
    if (frame < 0 || frame == state->shader_src_frame) {
        return;
    }

    int sw = 0, sh = 0;
    const uint8_t *pixels = xemu_texture_packs_animated_frame_pixels(
        state->shader_hash, NULL, frame, &sw, &sh);

    if (pixels == NULL) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, state->shader_src);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    state->shader_src_frame = frame;
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
static void render_texture_shader(XemuTexturePacksGLBindingState *state, int64_t now_us)
{
    if (!state->has_shader) {
        return;
    }

    /* ~60Hz. INT64_MIN on a fresh binding forces the first render. */
    if (state->shader_last_us != INT64_MIN &&
        now_us >= state->shader_last_us &&
        (now_us - state->shader_last_us) < 16000) {
        return;
    }
    state->shader_last_us = now_us;

    /* Live iChannel0 before sampling it. */
    refresh_shader_source(state, now_us);

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
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state->shader_fbo);
    glViewport(0, 0, state->shader_width, state->shader_height);
    glUseProgram(state->shader_program);

    glUniform1f(state->shader_u_time,
                (float)(now_us / 1000) / 1000.0f);
    glUniform2f(state->shader_u_resolution,
                (float)state->shader_width, (float)state->shader_height);
    glUniform1i(state->shader_u_frame, state->shader_frame);
    glUniform1i(state->shader_u_has_channel0, state->shader_src != 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state->shader_src);
    glUniform1i(state->shader_u_channel0, 0);

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

    state->shader_frame++;

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
    glBindTexture(GL_TEXTURE_2D, state->binding->gl_texture);
}

static void refresh_animated_state(XemuTexturePacksGLBindingState *state, int64_t now_us)
{
    if (state == NULL || !state->is_animated) {
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
    int frame = xemu_texture_packs_animated_frame_index(
        state->anim_hash, state->anim_variant, now_us);

    if (frame < 0 || frame == state->anim_frame) {
        return;
    }

    glBindTexture(state->binding->gl_target, state->binding->gl_texture);

    /*
     * Sub-image update: all frames share dimensions, so there is no need to
     * reallocate storage with glTexImage2D.
     */
    int uploaded = state->anim_frame;
    if (xemu_texture_packs_gl_upload_animated_frame(
            state->anim_hash, state->anim_variant, state->binding->gl_target,
            /*full_upload=*/false, /*regen_mips=*/state->anim_has_mips,
            now_us, &uploaded)) {
        state->anim_frame = uploaded;
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

void xemu_texture_packs_gl_refresh_dynamic(void)
{
    if (!xemu_texture_packs_dynamic_enabled() ||
        gl_binding_states == NULL ||
        g_hash_table_size(gl_binding_states) == 0) {
        return;
    }

    static int64_t last_refresh_us;
    int64_t now_us = xemu_texture_packs_anim_now_us();
    if (now_us >= last_refresh_us &&
        now_us - last_refresh_us < TEXTURE_ANIM_REFRESH_INTERVAL_US) {
        return;
    }
    last_refresh_us = now_us;

    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, gl_binding_states);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        XemuTexturePacksGLBindingState *state = value;
        if (state->is_animated) {
            refresh_animated_state(state, now_us);
        }
        if (state->has_shader) {
            reload_texture_shader_if_changed(state, now_us);
            render_texture_shader(state, now_us);
        }
    }
}


void xemu_texture_packs_gl_binding_created(TextureBinding *binding,
                                           uint64_t hash,
                                           int guest_width,
                                           int guest_height,
                                           bool animated,
                                           bool animated_has_mips)
{
    const char *shader_path = NULL;
    if (xemu_texture_packs_replace_enabled() && hash != 0 &&
        binding->gl_target == GL_TEXTURE_2D) {
        shader_path = xemu_texture_packs_get_shader_path(hash, NULL);
    }

    if (!animated && shader_path == NULL) {
        return;
    }

    XemuTexturePacksGLBindingState *state = gl_binding_state_create(binding);
    state->is_animated = animated;
    state->anim_hash = animated ? hash : 0;
    state->anim_variant = NULL;
    state->anim_frame = 0;
    state->anim_has_mips = animated && animated_has_mips;

    if (shader_path != NULL) {
        setup_texture_shader(state, hash, guest_width, guest_height);
        if (state->has_shader) {
            render_texture_shader(state, xemu_texture_packs_anim_now_us());
        }
    }

    if (!state->is_animated && !state->has_shader) {
        g_hash_table_remove(gl_binding_states, binding);
        g_free(state);
        if (g_hash_table_size(gl_binding_states) == 0) {
            g_hash_table_destroy(gl_binding_states);
            gl_binding_states = NULL;
        }
    }
}

void xemu_texture_packs_gl_binding_destroy(TextureBinding *binding)
{
    XemuTexturePacksGLBindingState *state = gl_binding_state_lookup(binding);
    if (state == NULL) {
        return;
    }

    g_free(state->anim_variant);
    if (state->shader_fbo) {
        glDeleteFramebuffers(1, &state->shader_fbo);
    }
    if (state->shader_program) {
        glDeleteProgram(state->shader_program);
    }
    if (state->shader_src) {
        glDeleteTextures(1, &state->shader_src);
    }

    g_hash_table_remove(gl_binding_states, binding);
    g_free(state);
    if (g_hash_table_size(gl_binding_states) == 0) {
        g_hash_table_destroy(gl_binding_states);
        gl_binding_states = NULL;
    }
}

bool xemu_texture_packs_gl_apply_sampler_override(TextureBinding *binding)
{
    XemuTexturePacksGLBindingState *state = gl_binding_state_lookup(binding);
    if (state == NULL || !state->has_shader) {
        return false;
    }

    glTexParameteri(binding->gl_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(binding->gl_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(binding->gl_target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(binding->gl_target, GL_TEXTURE_MAX_LEVEL, 0);
    binding->min_filter = 0xFFFFFFFF;
    binding->mag_filter = 0xFFFFFFFF;
    return true;
}

void xemu_texture_packs_gl_refresh_binding(TextureBinding *binding)
{
    if (gl_binding_states == NULL) {
        return;
    }
    XemuTexturePacksGLBindingState *state = gl_binding_state_lookup(binding);
    if (state != NULL && state->is_animated) {
        refresh_animated_state(state, xemu_texture_packs_anim_now_us());
    }
}

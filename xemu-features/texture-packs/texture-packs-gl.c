/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Adapter for the xemu NV2A OpenGL renderer. The renderer lineage includes:
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * Feature isolation/integration changes are part of the Joshua-1248 fork.
 */
/*
 * OpenGL adapter for the isolated texture-pack feature.
 */
#include "qemu/osdep.h"
#include <epoxy/gl.h>

#include "xemu-features/texture-packs/texture-packs.h"
#include "xemu-features/texture-packs/texture-packs-gl.h"

#include "hw/xbox/nv2a/pgraph/gl/renderer.h"

enum {
    MATERIAL_MAP_NORMAL = 0,
    MATERIAL_MAP_SPECULAR,
    MATERIAL_MAP_DISPLACEMENT,
    MATERIAL_MAP_AO,
    MATERIAL_MAP_COUNT,
};

static const char *const material_map_variants[MATERIAL_MAP_COUNT] = {
    "n", "s", "d", "ao",
};

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
    GLuint shader_material_map[MATERIAL_MAP_COUNT];
    bool shader_has_material_map[MATERIAL_MAP_COUNT];
    GLint shader_u_material_map[MATERIAL_MAP_COUNT];
    GLint shader_u_has_material_map[MATERIAL_MAP_COUNT];
    GLint shader_u_material_light_mode;
    GLint shader_u_material_normal_strength;
    GLint shader_u_material_ambient_strength;
    GLint shader_u_material_diffuse_strength;
    GLint shader_u_material_specular_strength;
    GLint shader_u_material_specular_power;
    GLint shader_u_material_parallax_scale;
    GLint shader_u_material_ao_strength;
    GLint shader_u_material_flip_normal_y;
    GLint shader_u_material_light_dir;
    GLint shader_u_material_view_dir;
    bool shader_builtin_material;
    uint64_t shader_material_revision;
    uint64_t shader_light_revision;
    bool shader_light_dirty;
    float shader_view_light_dir[3];
    int shader_width;
    int shader_height;
    int shader_frame;
    int64_t shader_last_us;
    bool shader_src_animated;
    int shader_src_frame;
    uint64_t shader_hash;
    XemuTexturePacksFileStamp shader_stamp;
    bool shader_stamp_valid;
    int64_t shader_check_us;
    bool timed_registered;
    guint timed_index;
} XemuTexturePacksGLBindingState;

static GHashTable *gl_binding_states;
static GPtrArray *gl_timed_states;

static XemuTexturePacksGLBindingState *gl_binding_state_lookup(TextureBinding *binding)
{
    return gl_binding_states != NULL ?
        g_hash_table_lookup(gl_binding_states, binding) : NULL;
}

static bool gl_state_needs_timed_refresh(
    const XemuTexturePacksGLBindingState *state)
{
    return state != NULL &&
           (state->is_animated ||
            (state->has_shader &&
             (!state->shader_builtin_material || state->shader_src_animated)));
}

static void gl_timed_state_register(XemuTexturePacksGLBindingState *state)
{
    if (!gl_state_needs_timed_refresh(state) || state->timed_registered) {
        return;
    }
    if (gl_timed_states == NULL) {
        gl_timed_states = g_ptr_array_new();
    }
    state->timed_index = gl_timed_states->len;
    state->timed_registered = true;
    g_ptr_array_add(gl_timed_states, state);
}

static void gl_timed_state_unregister(XemuTexturePacksGLBindingState *state)
{
    if (!state || !state->timed_registered || gl_timed_states == NULL) {
        return;
    }
    guint index = state->timed_index;
    guint last = gl_timed_states->len - 1;
    XemuTexturePacksGLBindingState *moved =
        g_ptr_array_index(gl_timed_states, last);
    g_ptr_array_remove_index_fast(gl_timed_states, index);
    state->timed_registered = false;
    if (index != last && moved != NULL) {
        moved->timed_index = index;
    }
    if (gl_timed_states->len == 0) {
        g_ptr_array_free(gl_timed_states, TRUE);
        gl_timed_states = NULL;
    }
}

static XemuTexturePacksGLBindingState *gl_binding_state_create(TextureBinding *binding)
{
    if (gl_binding_states == NULL) {
        gl_binding_states = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    XemuTexturePacksGLBindingState *state = g_new0(XemuTexturePacksGLBindingState, 1);
    state->binding = binding;
    state->anim_frame = 0;
    state->shader_view_light_dir[0] = 0.0f;
    state->shader_view_light_dir[1] = 0.0f;
    state->shader_view_light_dir[2] = 1.0f;
    state->shader_u_time = -1;
    state->shader_u_resolution = -1;
    state->shader_u_frame = -1;
    state->shader_u_has_channel0 = -1;
    state->shader_u_channel0 = -1;
    for (int i = 0; i < MATERIAL_MAP_COUNT; i++) {
        state->shader_u_material_map[i] = -1;
        state->shader_u_has_material_map[i] = -1;
    }
    state->shader_u_material_light_mode = -1;
    state->shader_u_material_normal_strength = -1;
    state->shader_u_material_ambient_strength = -1;
    state->shader_u_material_diffuse_strength = -1;
    state->shader_u_material_specular_strength = -1;
    state->shader_u_material_specular_power = -1;
    state->shader_u_material_parallax_scale = -1;
    state->shader_u_material_ao_strength = -1;
    state->shader_u_material_flip_normal_y = -1;
    state->shader_u_material_light_dir = -1;
    state->shader_u_material_view_dir = -1;
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
    "uniform sampler2D iNormalMap;\n"
    "uniform sampler2D iSpecularMap;\n"
    "uniform sampler2D iDisplacementMap;\n"
    "uniform sampler2D iAOMap;\n"
    "uniform bool iHasNormalMap;\n"
    "uniform bool iHasSpecularMap;\n"
    "uniform bool iHasDisplacementMap;\n"
    "uniform bool iHasAOMap;\n"
    "uniform int xemuMaterialLightMode;\n"
    "uniform float xemuMaterialNormalStrength;\n"
    "uniform float xemuMaterialAmbientStrength;\n"
    "uniform float xemuMaterialDiffuseStrength;\n"
    "uniform float xemuMaterialSpecularStrength;\n"
    "uniform float xemuMaterialSpecularPower;\n"
    "uniform float xemuMaterialParallaxScale;\n"
    "uniform float xemuMaterialAOStrength;\n"
    "uniform int xemuMaterialFlipNormalY;\n"
    "uniform vec3 xemuMaterialLightDir;\n"
    "uniform vec3 xemuMaterialViewDir;\n"
    "#line 1\n";

static const char *builtin_material_shader_body =
    "vec3 xemu_fetch_normal(vec2 t) {\n"
    "    if (!iHasNormalMap) {\n"
    "        return vec3(0.0, 0.0, 1.0);\n"
    "    }\n"
    "    vec3 n = texture(iNormalMap, t).xyz * 2.0 - 1.0;\n"
    "    if (xemuMaterialFlipNormalY != 0) {\n"
    "        n.y = -n.y;\n"
    "    }\n"
    "    n.xy *= xemuMaterialNormalStrength;\n"
    "    return normalize(vec3(n.xy, max(n.z, 0.0001)));\n"
    "}\n"
    "vec3 xemu_safe_dir(vec3 v) {\n"
    "    float len2 = dot(v, v);\n"
    "    return len2 > 0.000001 ? v * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);\n"
    "}\n"
    "vec2 xemu_parallax_uv(vec2 baseUV, vec3 viewTS) {\n"
    "    if (!iHasDisplacementMap || xemuMaterialParallaxScale <= 0.0) {\n"
    "        return baseUV;\n"
    "    }\n"
    "    float vz = max(viewTS.z, 0.06);\n"
    "    float grazingFade = smoothstep(0.055, 0.22, viewTS.z);\n"
    "    float layers = mix(16.0, 8.0, clamp(viewTS.z, 0.0, 1.0));\n"
    "    float layerDepth = 1.0 / layers;\n"
    "    vec2 ray = (viewTS.xy / vz) * (xemuMaterialParallaxScale * grazingFade);\n"
    "    vec2 delta = ray / layers;\n"
    "    vec2 tc = baseUV;\n"
    "    float currentLayer = 0.0;\n"
    "    float surfaceDepth = 1.0 - texture(iDisplacementMap, tc).r;\n"
    "    for (int i = 0; i < 16; ++i) {\n"
    "        if (float(i) >= layers || currentLayer >= surfaceDepth) {\n"
    "            break;\n"
    "        }\n"
    "        tc -= delta;\n"
    "        currentLayer += layerDepth;\n"
    "        surfaceDepth = 1.0 - texture(iDisplacementMap, tc).r;\n"
    "    }\n"
    "    vec2 prevTC = tc + delta;\n"
    "    float after = surfaceDepth - currentLayer;\n"
    "    float beforeDepth = 1.0 - texture(iDisplacementMap, prevTC).r;\n"
    "    float before = beforeDepth - (currentLayer - layerDepth);\n"
    "    float denom = after - before;\n"
    "    float weight = abs(denom) > 0.00001 ? clamp(after / denom, 0.0, 1.0) : 0.0;\n"
    "    return mix(tc, prevTC, weight);\n"
    "}\n"
    "void main() {\n"
    "    vec3 viewTS = xemu_safe_dir(xemuMaterialViewDir);\n"
    "    viewTS.z = max(viewTS.z, 0.0001);\n"
    "    viewTS = xemu_safe_dir(viewTS);\n"
    "    vec3 lightTS = xemu_safe_dir(xemuMaterialLightDir);\n"
    "    vec2 t = xemu_parallax_uv(uv, viewTS);\n"
    "    vec4 base = iHasChannel0 ? texture(iChannel0, t) : vec4(1.0);\n"
    "    vec3 n = xemu_fetch_normal(t);\n"
    "    float ao = 1.0;\n"
    "    if (iHasAOMap) {\n"
    "        ao = mix(1.0, texture(iAOMap, t).r, clamp(xemuMaterialAOStrength, 0.0, 1.0));\n"
    "    }\n"
    "    float ndotl = max(dot(n, lightTS), 0.0);\n"
    "    float lighting = xemuMaterialAmbientStrength + xemuMaterialDiffuseStrength * ndotl;\n"
    "    float specMask = iHasSpecularMap ? texture(iSpecularMap, t).r : 0.0;\n"
    "    vec3 halfTS = normalize(lightTS + viewTS);\n"
    "    float spec = 0.0;\n"
    "    if (specMask > 0.0 && xemuMaterialSpecularStrength > 0.0) {\n"
    "        spec = pow(max(dot(n, halfTS), 0.0), max(xemuMaterialSpecularPower, 1.0)) * specMask * xemuMaterialSpecularStrength;\n"
    "    }\n"
    "    vec3 color = base.rgb * lighting * ao + vec3(spec);\n"
    "    fragColor = vec4(clamp(color, 0.0, 1.0), base.a);\n"
    "}\n";

/*
 * Compile a fragment shader body against the shared texture-shader prologue.
 * User-authored .shader files and the built-in material-enhancement shader
 * both use this path.
 */
static GLuint compile_texture_shader_body(const char *label, const char *body)
{
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
        fprintf(stderr,
                "nv2a: texture-io: shader %s failed to compile:\n%s\n",
                label, log);
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
        fprintf(stderr,
                "nv2a: texture-io: shader %s failed to link:\n%s\n",
                label, log);
        glDeleteProgram(program);
        return 0;
    }

    fprintf(stderr, "nv2a: texture-io: loaded shader %s\n", label);
    return program;
}

static GLuint compile_texture_shader(const char *path)
{
    g_autofree char *body = NULL;
    gsize body_len = 0;

    if (!g_file_get_contents(path, &body, &body_len, NULL)) {
        fprintf(stderr, "nv2a: texture-io: could not read shader %s\n", path);
        return 0;
    }

    return compile_texture_shader_body(path, body);
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

    static const char *const sampler_names[MATERIAL_MAP_COUNT] = {
        "iNormalMap", "iSpecularMap", "iDisplacementMap", "iAOMap",
    };
    static const char *const present_names[MATERIAL_MAP_COUNT] = {
        "iHasNormalMap", "iHasSpecularMap", "iHasDisplacementMap",
        "iHasAOMap",
    };
    for (int i = 0; i < MATERIAL_MAP_COUNT; i++) {
        state->shader_u_material_map[i] =
            glGetUniformLocation(state->shader_program, sampler_names[i]);
        state->shader_u_has_material_map[i] =
            glGetUniformLocation(state->shader_program, present_names[i]);
    }
    state->shader_u_material_light_mode =
        glGetUniformLocation(state->shader_program, "xemuMaterialLightMode");
    state->shader_u_material_normal_strength =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialNormalStrength");
    state->shader_u_material_ambient_strength =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialAmbientStrength");
    state->shader_u_material_diffuse_strength =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialDiffuseStrength");
    state->shader_u_material_specular_strength =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialSpecularStrength");
    state->shader_u_material_specular_power =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialSpecularPower");
    state->shader_u_material_parallax_scale =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialParallaxScale");
    state->shader_u_material_ao_strength =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialAOStrength");
    state->shader_u_material_flip_normal_y =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialFlipNormalY");
    state->shader_u_material_light_dir =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialLightDir");
    state->shader_u_material_view_dir =
        glGetUniformLocation(state->shader_program,
                             "xemuMaterialViewDir");
}

static GLuint create_shader_material_map(uint64_t hash, const char *variant,
                                         bool *out_present)
{
    int width = 0, height = 0;
    const uint8_t *src = NULL;
    uint8_t *pixels = NULL;
    uint8_t fallback[4] = { 0, 0, 0, 255 };

    bool animated = xemu_texture_packs_replacement_is_animated(hash, variant);
    if (animated) {
        src = xemu_texture_packs_animated_frame_pixels(
            hash, variant, 0, &width, &height);
    } else {
        pixels = xemu_texture_packs_load_replacement_rgba_variant(
            hash, variant, &width, &height);
        src = pixels;
    }

    *out_present = src != NULL && width > 0 && height > 0;
    if (!*out_present) {
        width = 1;
        height = 1;
        if (strcmp(variant, "n") == 0) {
            fallback[0] = 128;
            fallback[1] = 128;
            fallback[2] = 255;
        } else if (strcmp(variant, "d") == 0) {
            fallback[0] = fallback[1] = fallback[2] = 128;
        } else if (strcmp(variant, "ao") == 0) {
            fallback[0] = fallback[1] = fallback[2] = 255;
        }
        src = fallback;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, src);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    if (pixels != NULL) {
        xemu_texture_packs_free_pixels(pixels);
    }
    return tex;
}

static bool has_builtin_material_shader(uint64_t hash)
{
    return xemu_texture_packs_material_enhancement_enabled() &&
           xemu_texture_packs_material_sidecars_present(hash);
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
    bool builtin_material = has_builtin_material_shader(hash);
    GLuint program = 0;

    /* Material Enhancement is an explicit UI mode. When enabled and a
     * material sidecar exists, it must win over a legacy per-texture .shader
     * or the UI controls would appear to do nothing. */
    if (builtin_material) {
        program = compile_texture_shader_body("<built-in material enhancer>",
                                              builtin_material_shader_body);
    } else if (path != NULL) {
        program = compile_texture_shader(path);
    }

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

    for (int i = 0; i < MATERIAL_MAP_COUNT; i++) {
        state->shader_material_map[i] = create_shader_material_map(
            hash, material_map_variants[i], &state->shader_has_material_map[i]);
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
        for (int i = 0; i < MATERIAL_MAP_COUNT; i++) {
            if (state->shader_material_map[i]) {
                glDeleteTextures(1, &state->shader_material_map[i]);
                state->shader_material_map[i] = 0;
            }
            state->shader_has_material_map[i] = false;
        }
        return;
    }

    state->has_shader = true;
    state->shader_builtin_material = builtin_material;
    state->shader_material_revision = 0;
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

    state->shader_stamp_valid =
        path != NULL && xemu_texture_packs_get_file_stamp(path, &state->shader_stamp);
    state->shader_check_us = INT64_MIN;
}

/*
 * Recompile the shader when its file changes on disk.
 *
 * The old program is kept until the new one links successfully, so a syntax
 * error while editing leaves the last working shader on screen (with the
 * error logged) instead of blanking the texture.
 *
 * File metadata is checked at most a few times a second: this runs from the
 * per-bind sweep, and doing filesystem work per draw call would be its own
 * performance problem.
 */
static void reload_texture_shader_if_changed(XemuTexturePacksGLBindingState *state,
                                             int64_t now_us)
{
    if (!state->has_shader || state->shader_builtin_material) {
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

    XemuTexturePacksFileStamp stamp;
    if (!xemu_texture_packs_get_file_stamp(path, &stamp)) {
        return;
    }

    if (state->shader_stamp_valid &&
        xemu_texture_packs_file_stamp_equal(&stamp, &state->shader_stamp)) {
        return;
    }

    /* Record the observed save before compiling. A broken edit is therefore
     * tried once, while the next actual save (including a rapid same-second
     * save) gets a new high-resolution stamp and retries immediately. */
    state->shader_stamp = stamp;
    state->shader_stamp_valid = true;

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
static void apply_material_shader_uniforms(
    XemuTexturePacksGLBindingState *state)
{
    XemuTexturePacksMaterialConfig cfg;
    xemu_texture_packs_get_material_config(&cfg);

    if (state->shader_u_material_light_mode >= 0) {
        glUniform1i(state->shader_u_material_light_mode, cfg.light_mode);
    }
    if (state->shader_u_material_normal_strength >= 0) {
        glUniform1f(state->shader_u_material_normal_strength,
                    cfg.normal_strength);
    }
    if (state->shader_u_material_ambient_strength >= 0) {
        glUniform1f(state->shader_u_material_ambient_strength,
                    cfg.ambient_strength);
    }
    if (state->shader_u_material_diffuse_strength >= 0) {
        glUniform1f(state->shader_u_material_diffuse_strength,
                    cfg.diffuse_strength);
    }
    if (state->shader_u_material_specular_strength >= 0) {
        glUniform1f(state->shader_u_material_specular_strength,
                    cfg.specular_strength);
    }
    if (state->shader_u_material_specular_power >= 0) {
        glUniform1f(state->shader_u_material_specular_power,
                    cfg.specular_power);
    }
    if (state->shader_u_material_parallax_scale >= 0) {
        glUniform1f(state->shader_u_material_parallax_scale,
                    cfg.parallax_scale);
    }
    if (state->shader_u_material_ao_strength >= 0) {
        glUniform1f(state->shader_u_material_ao_strength,
                    cfg.ao_strength);
    }
    if (state->shader_u_material_flip_normal_y >= 0) {
        glUniform1i(state->shader_u_material_flip_normal_y,
                    cfg.flip_normal_y ? 1 : 0);
    }
    if (state->shader_u_material_light_dir >= 0) {
        const float *dir = cfg.light_mode ==
                XEMU_TEXTURE_PACKS_MATERIAL_LIGHT_HEADLIGHT
            ? state->shader_view_light_dir : cfg.light_dir;
        glUniform3f(state->shader_u_material_light_dir,
                    dir[0], dir[1], dir[2]);
    }
    if (state->shader_u_material_view_dir >= 0) {
        glUniform3f(state->shader_u_material_view_dir,
                    state->shader_view_light_dir[0],
                    state->shader_view_light_dir[1],
                    state->shader_view_light_dir[2]);
    }
}

static bool update_material_hash_light(XemuTexturePacksGLBindingState *state)
{
    if (state == NULL || !state->shader_builtin_material) {
        return false;
    }

    float dir[3];
    uint64_t revision = 0;
    xemu_texture_packs_material_get_hash_view_light(
        state->shader_hash, dir, &revision);
    if (revision == 0 || revision == state->shader_light_revision) {
        return false;
    }

    memcpy(state->shader_view_light_dir, dir, sizeof(dir));
    state->shader_light_revision = revision;
    state->shader_light_dirty = true;
    return true;
}

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

    /* The caller has already snapshotted texture-unit state and selected unit
     * 0. Preserve pixel-store state too: the NV2A upload path may rely on
     * non-default unpack settings after this helper returns. */
    GLint prev_unpack_row_length = 0;
    GLint prev_unpack_alignment = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_unpack_row_length);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_alignment);

    glBindTexture(GL_TEXTURE_2D, state->shader_src);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_unpack_row_length);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_alignment);

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

    /* ~60Hz for time-driven shaders, but material controls must refresh
     * immediately even when QEMU_CLOCK_VIRTUAL is stopped by a paused UI. */
    uint64_t material_revision =
        state->shader_builtin_material ?
            xemu_texture_packs_material_config_revision() : 0;
    bool material_changed = state->shader_builtin_material &&
        material_revision != state->shader_material_revision;

    if (state->shader_builtin_material) {
        bool needs_render = material_changed || state->shader_light_dirty ||
                            state->shader_src_animated;
        if (!needs_render) {
            return;
        }
        /* Camera light is draw-synchronous. Do not add a 16 ms throttle to a
         * real camera/config change: differently oriented draws may reuse the
         * same replacement hash and each must be able to refresh before it is
         * sampled. Only a purely time-driven animated source keeps the shader
         * rate cap. */
        if (!material_changed && !state->shader_light_dirty &&
            state->shader_src_animated && state->shader_last_us != INT64_MIN &&
            now_us >= state->shader_last_us &&
            (now_us - state->shader_last_us) < 16000) {
            return;
        }
    } else if (state->shader_last_us != INT64_MIN &&
               now_us >= state->shader_last_us &&
               (now_us - state->shader_last_us) < 16000) {
        return;
    }
    state->shader_last_us = now_us;

    /* --- save state --- */
    GLint prev_fbo = 0, prev_program = 0, prev_vao = 0, prev_active = 0;
    GLint prev_viewport[4];
    GLint prev_texture_binding[1 + MATERIAL_MAP_COUNT] = { 0 };
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    for (int i = 0; i < 1 + MATERIAL_MAP_COUNT; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture_binding[i]);
    }
    glActiveTexture(prev_active);

    GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prev_blend = glIsEnabled(GL_BLEND);
    GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean prev_rasterizer_discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    GLboolean prev_framebuffer_srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLboolean prev_color_mask[4];
    GLint prev_polygon_mode[2];
    glGetBooleanv(GL_COLOR_WRITEMASK, prev_color_mask);
    glGetIntegerv(GL_POLYGON_MODE, prev_polygon_mode);

    /* This helper can run immediately before a guest geometry draw. Do not
     * inherit that draw's channel mask, wireframe mode, rasterizer discard or
     * framebuffer-sRGB state into the material prepass. */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Live iChannel0 before sampling it. Do this only after the renderer's
     * texture-unit bindings have been captured; otherwise glBindTexture()
     * would overwrite the current NV2A unit before we knew what to restore. */
    glActiveTexture(GL_TEXTURE0);
    refresh_shader_source(state, now_us);

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

    for (int i = 0; i < MATERIAL_MAP_COUNT; i++) {
        glActiveTexture(GL_TEXTURE1 + i);
        glBindTexture(GL_TEXTURE_2D, state->shader_material_map[i]);
        glUniform1i(state->shader_u_material_map[i], 1 + i);
        glUniform1i(state->shader_u_has_material_map[i],
                    state->shader_has_material_map[i] ? 1 : 0);
    }
    apply_material_shader_uniforms(state);

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
    if (state->shader_builtin_material) {
        state->shader_material_revision = material_revision;
        state->shader_light_dirty = false;
    }

    /* --- restore state --- */
    glBindVertexArray(prev_vao);
    glUseProgram(prev_program);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
               prev_viewport[3]);

    glColorMask(prev_color_mask[0], prev_color_mask[1],
                prev_color_mask[2], prev_color_mask[3]);
    glPolygonMode(GL_FRONT, prev_polygon_mode[0]);
    glPolygonMode(GL_BACK, prev_polygon_mode[1]);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (prev_blend) glEnable(GL_BLEND);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    if (prev_rasterizer_discard) glEnable(GL_RASTERIZER_DISCARD);
    if (prev_framebuffer_srgb) glEnable(GL_FRAMEBUFFER_SRGB);

    for (int i = 0; i < 1 + MATERIAL_MAP_COUNT; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_texture_binding[i]);
    }
    glActiveTexture(prev_active);
}

static void refresh_material_stage_for_draw(void *opaque, int stage,
                                            uint64_t hash)
{
    PGRAPHState *pg = opaque;
    if (pg == NULL || pg->gl_renderer_state == NULL || hash == 0 ||
        stage < 0 || stage >= NV2A_MAX_TEXTURES) {
        return;
    }

    /* GL binds the NV2A textures at BEGIN, before the draw's complete vertex
     * data is available to the geometry observer. At END the observer now has
     * the correct per-draw TBN, so refresh exactly the binding that stage will
     * sample before pgraph_gl_flush_draw() issues the geometry draw. */
    PGRAPHGLState *r = pg->gl_renderer_state;
    TextureBinding *binding = r->texture_binding[stage];
    XemuTexturePacksGLBindingState *state = gl_binding_state_lookup(binding);
    if (state == NULL || !state->has_shader || !state->shader_builtin_material ||
        state->shader_hash != hash) {
        return;
    }

    if (update_material_hash_light(state)) {
        render_texture_shader(state, xemu_texture_packs_anim_now_us());
    }
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
    /* Only states with genuine time/file-driven work live in this compact
     * array. Static built-in material bindings are refreshed by exact draw
     * binding/config events and never participate in the 250 Hz sweep. */
    if (!xemu_texture_packs_dynamic_enabled() || gl_timed_states == NULL ||
        gl_timed_states->len == 0) {
        return;
    }

    static int64_t last_refresh_us;
    int64_t now_us = xemu_texture_packs_anim_now_us();
    if (now_us >= last_refresh_us &&
        now_us - last_refresh_us < TEXTURE_ANIM_REFRESH_INTERVAL_US) {
        return;
    }
    last_refresh_us = now_us;

    for (guint i = 0; i < gl_timed_states->len; ++i) {
        XemuTexturePacksGLBindingState *state =
            g_ptr_array_index(gl_timed_states, i);
        if (state->is_animated) {
            refresh_animated_state(state, now_us);
        }
        if (state->has_shader) {
            reload_texture_shader_if_changed(state, now_us);
            (void)update_material_hash_light(state);
            render_texture_shader(state, now_us);
        }
    }
}



bool xemu_texture_packs_gl_bound_hash(void *opaque, int stage,
                                      uint64_t *out_hash)
{
    if (out_hash) {
        *out_hash = 0;
    }
    PGRAPHState *pg = opaque;
    if (!pg || !out_hash || stage < 0 || stage >= NV2A_MAX_TEXTURES ||
        !pg->gl_renderer_state) {
        return false;
    }
    PGRAPHGLState *r = pg->gl_renderer_state;
    TextureBinding *binding = r->texture_binding[stage];
    if (!binding) {
        return false;
    }
    *out_hash = binding->data_hash;
    return true;
}

void xemu_texture_packs_gl_binding_created(TextureBinding *binding,
                                           uint64_t hash,
                                           int guest_width,
                                           int guest_height,
                                           bool animated,
                                           bool animated_has_mips)
{
    /* GL texture binding occurs at NV097 BEGIN, before complete geometry for
     * the draw exists. Register the feature-owned END-time refresh callback so
     * the geometry observer can update exactly this draw before flush_draw. */
    xemu_texture_packs_material_set_draw_refresh_callback(
        refresh_material_stage_for_draw);
    const char *shader_path = NULL;
    bool builtin_material = false;
    if (xemu_texture_packs_replace_enabled() && hash != 0 &&
        binding->gl_target == GL_TEXTURE_2D) {
        shader_path = xemu_texture_packs_get_shader_path(hash, NULL);
        builtin_material = xemu_texture_packs_material_enhancement_enabled() &&
            xemu_texture_packs_material_sidecars_present(hash);
    }

    if (!animated && shader_path == NULL && !builtin_material) {
        return;
    }

    XemuTexturePacksGLBindingState *state = gl_binding_state_create(binding);
    state->is_animated = animated;
    state->anim_hash = animated ? hash : 0;
    state->anim_variant = NULL;
    state->anim_frame = 0;
    state->anim_has_mips = animated && animated_has_mips;

    if (shader_path != NULL || builtin_material) {
        setup_texture_shader(state, hash, guest_width, guest_height);
        if (state->has_shader) {
            (void)update_material_hash_light(state);
            render_texture_shader(state, xemu_texture_packs_anim_now_us());
        }
    }

    gl_timed_state_register(state);

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

    gl_timed_state_unregister(state);
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
    for (int i = 0; i < MATERIAL_MAP_COUNT; i++) {
        if (state->shader_material_map[i]) {
            glDeleteTextures(1, &state->shader_material_map[i]);
        }
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
    if (state == NULL) {
        return;
    }

    if (state->is_animated) {
        refresh_animated_state(state, xemu_texture_packs_anim_now_us());
    }
    if (state->has_shader && state->shader_builtin_material) {
        (void)update_material_hash_light(state);
        uint64_t config_revision = xemu_texture_packs_material_config_revision();
        if (state->shader_light_dirty ||
            state->shader_material_revision != config_revision) {
            /* Configuration is rare and draw-synchronous light updates are
             * handled again at END. Acquire time only when a render is needed. */
            render_texture_shader(state, xemu_texture_packs_anim_now_us());
        }
    }
}

/*
 * Vulkan adapter for the isolated texture-pack feature.
 */
#include "qemu/osdep.h"
#include <glib/gstdio.h>

#include "xemu-features/texture-packs/texture-packs.h"
#include "xemu-features/texture-packs/texture-packs-vk.h"
#include "hw/xbox/nv2a/pgraph/vk/renderer.h"

typedef struct XemuTexturePacksVKBindingState {
    TextureBinding *binding;
    bool replaced;
    uint32_t replacement_width;
    uint32_t replacement_height;
    bool is_animated;
    int anim_frame;

    bool has_shader;
    VkRenderPass shader_render_pass;
    VkFramebuffer shader_framebuffer;
    VkPipeline shader_pipeline;
    VkPipelineLayout shader_pipeline_layout;
    VkDescriptorSetLayout shader_ds_layout;
    VkDescriptorPool shader_ds_pool;
    VkDescriptorSet shader_ds;
    VkSampler shader_sampler;
    VkImage shader_src_image;
    VkImageView shader_src_view;
    VmaAllocation shader_src_alloc;
    ShaderModuleInfo *shader_vs;
    ShaderModuleInfo *shader_fs;
    uint32_t shader_width;
    uint32_t shader_height;
    int shader_frame;
    int64_t shader_last_us;
    bool shader_src_animated;
    int shader_src_frame;
    uint32_t shader_src_width;
    uint32_t shader_src_height;
    int64_t shader_mtime;
    int64_t shader_check_us;
    uint64_t shader_hash;
} XemuTexturePacksVKBindingState;

static GHashTable *vk_binding_states;

static XemuTexturePacksVKBindingState *vk_state_lookup(TextureBinding *binding)
{
    return vk_binding_states != NULL ?
        g_hash_table_lookup(vk_binding_states, binding) : NULL;
}

static XemuTexturePacksVKBindingState *vk_state_create(TextureBinding *binding)
{
    if (vk_binding_states == NULL) {
        vk_binding_states = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    XemuTexturePacksVKBindingState *state =
        g_new0(XemuTexturePacksVKBindingState, 1);
    state->binding = binding;
    state->anim_frame = 0;
    state->shader_render_pass = VK_NULL_HANDLE;
    state->shader_framebuffer = VK_NULL_HANDLE;
    state->shader_pipeline = VK_NULL_HANDLE;
    state->shader_pipeline_layout = VK_NULL_HANDLE;
    state->shader_ds_layout = VK_NULL_HANDLE;
    state->shader_ds_pool = VK_NULL_HANDLE;
    state->shader_sampler = VK_NULL_HANDLE;
    state->shader_src_image = VK_NULL_HANDLE;
    state->shader_src_view = VK_NULL_HANDLE;
    state->shader_vs = NULL;
    state->shader_fs = NULL;
    state->shader_last_us = INT64_MIN;
    state->shader_check_us = INT64_MIN;
    g_hash_table_insert(vk_binding_states, binding, state);
    return state;
}

static uint32_t replacement_mip_levels(uint32_t w, uint32_t h)
{
    uint32_t levels = 1;
    uint32_t d = MAX(w, h);
    while (d > 1) {
        d >>= 1;
        levels++;
    }
    return levels;
}

/* Per-level barrier; the shared helper always covers every mip level. */
static void transition_mip_level(VkCommandBuffer cmd, VkImage image,
                                 uint32_t level, VkImageLayout old_layout,
                                 VkImageLayout new_layout,
                                 VkAccessFlags src_access,
                                 VkAccessFlags dst_access,
                                 VkPipelineStageFlags src_stage,
                                 VkPipelineStageFlags dst_stage,
                                 uint32_t layer_count)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = level,
        .subresourceRange.levelCount = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = layer_count,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
}

/*
 * Upload a replacement image and generate its mip chain.
 *
 * The VkImage was created at the replacement's dimensions with format
 * R8G8B8A8_UNORM. Replacements can exceed the shared staging buffer, so a
 * dedicated staging buffer is allocated when needed; this only happens for
 * large replacements and is freed immediately afterwards.
 */
static void upload_replacement_image(PGRAPHState *pg, XemuTexturePacksVKBindingState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const bool cubemap = state->binding->key.state.cubemap;
    const int num_layers = cubemap ? 6 : 1;

    const int w = (int)state->replacement_width;
    const int h = (int)state->replacement_height;
    const size_t layer_size = (size_t)w * h * 4;
    const size_t size = layer_size * num_layers;

    /*
     * Load every layer up front so a mid-way failure does not leave the
     * image partially written.
     */
    uint8_t *layer_pixels[6] = { NULL };
    /*
     * Animated frames are borrowed from texture-io's decoded cache rather
     * than allocated here, so track which pointers we actually own and must
     * free afterwards.
     */
    bool owns_pixels[6] = { false };
    bool load_ok = true;

    for (int i = 0; i < num_layers && load_ok; i++) {
        int lw = 0, lh = 0;
        const char *variant =
            cubemap ? xemu_texture_packs_cubemap_face_name(i) : NULL;

        if (state->is_animated) {
            /*
             * Decoded frames are already in memory; take the one due now
             * instead of reloading (and re-decoding frame 0) from disk.
             */
            const uint8_t *frame_pixels =
                xemu_texture_packs_animated_frame_pixels(
                    state->binding->hash, variant, state->anim_frame, &lw, &lh);
            layer_pixels[i] = (uint8_t *)frame_pixels;
            owns_pixels[i] = false;
        } else {
            layer_pixels[i] = xemu_texture_packs_load_replacement_rgba_variant(
                state->binding->hash, variant, &lw, &lh);
            owns_pixels[i] = layer_pixels[i] != NULL;
        }

        if (layer_pixels[i] == NULL || lw != w || lh != h) {
            /* Missing, undecodable, or changed on disk since sizing. */
            load_ok = false;
        }
    }

    if (!load_ok) {
        for (int i = 0; i < num_layers; i++) {
            if (layer_pixels[i] && owns_pixels[i]) {
                xemu_texture_packs_free_pixels(layer_pixels[i]);
            }
        }
        return;
    }

    VkBuffer src_buffer = r->storage_buffers[BUFFER_STAGING_SRC].buffer;
    VmaAllocation src_alloc =
        r->storage_buffers[BUFFER_STAGING_SRC].allocation;

    VkBuffer temp_buffer = VK_NULL_HANDLE;
    VmaAllocation temp_alloc = VK_NULL_HANDLE;
    bool using_temp = size > r->storage_buffers[BUFFER_STAGING_SRC].buffer_size;

    if (using_temp) {
        VkBufferCreateInfo buffer_create_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo alloc_create_info = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
        };

        if (vmaCreateBuffer(r->allocator, &buffer_create_info,
                            &alloc_create_info, &temp_buffer, &temp_alloc,
                            NULL) != VK_SUCCESS) {
            fprintf(stderr,
                    "nv2a: texture-io: could not allocate %zu byte staging "
                    "buffer for replacement %016" PRIx64 "\n",
                    size, state->binding->hash);
            for (int i = 0; i < num_layers; i++) {
                if (owns_pixels[i]) {
                    xemu_texture_packs_free_pixels(layer_pixels[i]);
                }
            }
            return;
        }

        src_buffer = temp_buffer;
        src_alloc = temp_alloc;
    }

    uint8_t *mapped_memory_ptr;
    VK_CHECK(vmaMapMemory(r->allocator, src_alloc, (void *)&mapped_memory_ptr));
    for (int i = 0; i < num_layers; i++) {
        memcpy(mapped_memory_ptr + (size_t)i * layer_size, layer_pixels[i],
               layer_size);
    }
    vmaFlushAllocation(r->allocator, src_alloc, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(r->allocator, src_alloc);

    for (int i = 0; i < num_layers; i++) {
        if (owns_pixels[i]) {
            xemu_texture_packs_free_pixels(layer_pixels[i]);
        }
    }

    const uint32_t mip_levels = replacement_mip_levels(w, h);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = src_buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    /* Level 0: copy from staging. */
    transition_mip_level(cmd, state->binding->image, 0, state->binding->current_layout,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         num_layers);

    VkBufferImageCopy regions[6];
    for (int i = 0; i < num_layers; i++) {
        regions[i] = (VkBufferImageCopy){
            .bufferOffset = (VkDeviceSize)i * layer_size,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .imageSubresource.mipLevel = 0,
            .imageSubresource.baseArrayLayer = i,
            .imageSubresource.layerCount = 1,
            .imageOffset = (VkOffset3D){ 0, 0, 0 },
            .imageExtent = (VkExtent3D){ w, h, 1 },
        };
    }
    vkCmdCopyBufferToImage(cmd, src_buffer, state->binding->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, num_layers,
                           regions);

    /* Successively blit each level down by half. */
    int32_t mip_w = w, mip_h = h;

    for (uint32_t level = 1; level < mip_levels; level++) {
        int32_t next_w = mip_w > 1 ? mip_w / 2 : 1;
        int32_t next_h = mip_h > 1 ? mip_h / 2 : 1;

        /* Source level: DST -> SRC */
        transition_mip_level(cmd, state->binding->image, level - 1,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         num_layers);

        /* Destination level: UNDEFINED -> DST */
        transition_mip_level(cmd, state->binding->image, level,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         num_layers);

        VkImageBlit blit = {
            .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .srcSubresource.mipLevel = level - 1,
            .srcSubresource.baseArrayLayer = 0,
            .srcSubresource.layerCount = num_layers,
            .srcOffsets[0] = (VkOffset3D){ 0, 0, 0 },
            .srcOffsets[1] = (VkOffset3D){ mip_w, mip_h, 1 },
            .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .dstSubresource.mipLevel = level,
            .dstSubresource.baseArrayLayer = 0,
            .dstSubresource.layerCount = num_layers,
            .dstOffsets[0] = (VkOffset3D){ 0, 0, 0 },
            .dstOffsets[1] = (VkOffset3D){ next_w, next_h, 1 },
        };

        vkCmdBlitImage(cmd, state->binding->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state->binding->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);

        /* Source level is finished: SRC -> shader read */
        transition_mip_level(cmd, state->binding->image, level - 1,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_ACCESS_TRANSFER_READ_BIT,
                             VK_ACCESS_SHADER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         num_layers);

        mip_w = next_w;
        mip_h = next_h;
    }

    /* Final level is still TRANSFER_DST. */
    transition_mip_level(cmd, state->binding->image, mip_levels - 1,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         num_layers);

    state->binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_4);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    if (using_temp) {
        vmaDestroyBuffer(r->allocator, temp_buffer, temp_alloc);
    }
}

/*
 * ---------------------------------------------------------------------
 * Procedural shader replacements (<hash>.shader) -- Vulkan
 * ---------------------------------------------------------------------
 *
 * Same file format and uniform set as the GL backend. The shader is drawn
 * into the texture's own VkImage through a dedicated single-attachment
 * render pass whose finalLayout is SHADER_READ_ONLY_OPTIMAL, so the render
 * pass itself performs the layout transition and no explicit barriers are
 * needed around the draw.
 */

#define TEXTURE_SHADER_MAX_SIZE 4096
#define TEXTURE_SHADER_INTERVAL_US 16000 /* ~60Hz */

typedef struct TextureShaderPushConstants {
    float iTime;
    float iResolution[2];
    int32_t iFrame;
    int32_t iHasChannel0;
} TextureShaderPushConstants;

static const char *vk_texture_shader_vs_src =
    "#version 450 core\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/*
 * The push-constant block is aliased to plain names with #define so a single
 * .shader file works unmodified on both backends, where GL exposes these as
 * ordinary uniforms.
 */
static const char *vk_texture_shader_fs_prologue =
    "#version 450 core\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "layout(set = 0, binding = 0) uniform sampler2D iChannel0;\n"
    "layout(push_constant) uniform PC {\n"
    "    float iTime;\n"
    "    vec2 iResolution;\n"
    "    int iFrame;\n"
    "    int iHasChannel0;\n"
    "} pc;\n"
    "#define iTime pc.iTime\n"
    "#define iResolution pc.iResolution\n"
    "#define iFrame pc.iFrame\n"
    "#define iHasChannel0 (pc.iHasChannel0 != 0)\n"
    "#line 1\n";

static void destroy_texture_shader(PGRAPHVkState *r, XemuTexturePacksVKBindingState *state)
{
    if (state->shader_framebuffer) {
        vkDestroyFramebuffer(r->device, state->shader_framebuffer, NULL);
    }
    if (state->shader_pipeline) {
        vkDestroyPipeline(r->device, state->shader_pipeline, NULL);
    }
    if (state->shader_pipeline_layout) {
        vkDestroyPipelineLayout(r->device, state->shader_pipeline_layout,
                                NULL);
    }
    if (state->shader_render_pass) {
        vkDestroyRenderPass(r->device, state->shader_render_pass, NULL);
    }
    if (state->shader_ds_pool) {
        vkDestroyDescriptorPool(r->device, state->shader_ds_pool, NULL);
    }
    if (state->shader_ds_layout) {
        vkDestroyDescriptorSetLayout(r->device, state->shader_ds_layout,
                                     NULL);
    }
    if (state->shader_sampler) {
        vkDestroySampler(r->device, state->shader_sampler, NULL);
    }
    if (state->shader_src_view) {
        vkDestroyImageView(r->device, state->shader_src_view, NULL);
    }
    if (state->shader_src_image) {
        vmaDestroyImage(r->allocator, state->shader_src_image,
                        state->shader_src_alloc);
    }
    if (state->shader_vs) {
        pgraph_vk_destroy_shader_module(r, state->shader_vs);
    }
    if (state->shader_fs) {
        pgraph_vk_destroy_shader_module(r, state->shader_fs);
    }

    state->has_shader = false;
    state->shader_framebuffer = VK_NULL_HANDLE;
    state->shader_pipeline = VK_NULL_HANDLE;
    state->shader_pipeline_layout = VK_NULL_HANDLE;
    state->shader_render_pass = VK_NULL_HANDLE;
    state->shader_ds_pool = VK_NULL_HANDLE;
    state->shader_ds_layout = VK_NULL_HANDLE;
    state->shader_sampler = VK_NULL_HANDLE;
    state->shader_src_view = VK_NULL_HANDLE;
    state->shader_src_image = VK_NULL_HANDLE;
    state->shader_vs = NULL;
    state->shader_fs = NULL;
}

/*
 * Create and populate the iChannel0 source image.
 *
 * A 1x1 transparent image is created when the hash has no image replacement,
 * so the descriptor set always has something valid bound; shaders gate on
 * iHasChannel0 rather than on the descriptor being absent.
 */
static bool create_shader_source_image(PGRAPHState *pg,
                                       XemuTexturePacksVKBindingState *state,
                                       uint64_t hash, bool *out_has_source)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int sw = 0, sh = 0;
    uint8_t *pixels = NULL;
    const uint8_t *src = NULL;
    const uint8_t transparent[4] = { 0, 0, 0, 0 };

    bool animated = xemu_texture_packs_replacement_is_animated(hash, NULL);

    if (animated) {
        /*
         * Borrowed from the decoded frame cache; not freed here. Also the
         * only path that works for WebP, which stbi_load cannot read.
         */
        src = xemu_texture_packs_animated_frame_pixels(hash, NULL, 0, &sw, &sh);
    } else {
        pixels = xemu_texture_packs_load_replacement_rgba(hash, &sw, &sh);
        src = pixels;
    }

    state->shader_src_animated = animated;
    state->shader_src_frame = 0;

    if (src == NULL || sw <= 0 || sh <= 0) {
        sw = 1;
        sh = 1;
        src = transparent;
        state->shader_src_animated = false;
        *out_has_source = false;
    } else {
        *out_has_source = true;
    }

    state->shader_src_width = (uint32_t)sw;
    state->shader_src_height = (uint32_t)sh;

    const size_t size = (size_t)sw * sh * 4;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = { (uint32_t)sw, (uint32_t)sh, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo aci = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    if (vmaCreateImage(r->allocator, &ici, &aci, &state->shader_src_image,
                       &state->shader_src_alloc, NULL) != VK_SUCCESS) {
        if (pixels) {
            xemu_texture_packs_free_pixels(pixels);
        }
        return false;
    }

    /* Staging upload. Source images are small; the shared buffer suffices. */
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation buf_alloc = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo bac = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    };

    if (vmaCreateBuffer(r->allocator, &bci, &bac, &buf, &buf_alloc, NULL) !=
        VK_SUCCESS) {
        if (pixels) {
            xemu_texture_packs_free_pixels(pixels);
        }
        return false;
    }

    uint8_t *mapped = NULL;
    VK_CHECK(vmaMapMemory(r->allocator, buf_alloc, (void *)&mapped));
    memcpy(mapped, src, size);
    vmaFlushAllocation(r->allocator, buf_alloc, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(r->allocator, buf_alloc);

    if (pixels) {
        xemu_texture_packs_free_pixels(pixels);
    }

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);

    transition_mip_level(cmd, state->shader_src_image, 0,
                         VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 1);

    VkBufferImageCopy region = {
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageExtent = { (uint32_t)sw, (uint32_t)sh, 1 },
    };
    vkCmdCopyBufferToImage(cmd, buf, state->shader_src_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition_mip_level(cmd, state->shader_src_image, 0,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1);

    pgraph_vk_end_single_time_commands(pg, cmd);
    vmaDestroyBuffer(r->allocator, buf, buf_alloc);

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = state->shader_src_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };

    return vkCreateImageView(r->device, &ivci, NULL,
                             &state->shader_src_view) == VK_SUCCESS;
}

/*
 * Build everything needed to render a binding's shader. Called once, after
 * the image and view exist. Any failure tears down cleanly and leaves
 * has_shader false, so a broken shader degrades to a normal texture rather
 * than breaking the frame.
 */
static void setup_texture_shader(PGRAPHState *pg, XemuTexturePacksVKBindingState *state,
                                 uint64_t hash, uint32_t width,
                                 uint32_t height, VkFormat format)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const char *path = xemu_texture_packs_get_shader_path(hash, NULL);
    if (path == NULL) {
        return;
    }

    g_autofree char *body = NULL;
    gsize body_len = 0;
    if (!g_file_get_contents(path, &body, &body_len, NULL)) {
        fprintf(stderr, "nv2a: texture-io: could not read shader %s\n", path);
        return;
    }

    g_autofree char *fs_src =
        g_strconcat(vk_texture_shader_fs_prologue, body, NULL);

    state->shader_width = CLAMP(width, 1, TEXTURE_SHADER_MAX_SIZE);
    state->shader_height = CLAMP(height, 1, TEXTURE_SHADER_MAX_SIZE);
    state->shader_frame = 0;
    state->shader_last_us = INT64_MIN;

    state->shader_vs = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, vk_texture_shader_vs_src);
    state->shader_fs = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, fs_src);

    if (state->shader_vs == NULL || state->shader_fs == NULL ||
        state->shader_vs->module == VK_NULL_HANDLE ||
        state->shader_fs->module == VK_NULL_HANDLE) {
        fprintf(stderr, "nv2a: texture-io: shader %s failed to compile\n",
                path);
        destroy_texture_shader(r, state);
        return;
    }

    bool has_source = false;
    if (!create_shader_source_image(pg, state, hash, &has_source)) {
        fprintf(stderr,
                "nv2a: texture-io: could not create shader source image "
                "for %s\n", path);
        destroy_texture_shader(r, state);
        return;
    }

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = 0.0f,
    };
    if (vkCreateSampler(r->device, &sci, NULL, &state->shader_sampler) !=
        VK_SUCCESS) {
        destroy_texture_shader(r, state);
        return;
    }

    VkDescriptorSetLayoutBinding dslb = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &dslb,
    };
    if (vkCreateDescriptorSetLayout(r->device, &dslci, NULL,
                                    &state->shader_ds_layout) !=
        VK_SUCCESS) {
        destroy_texture_shader(r, state);
        return;
    }

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    if (vkCreateDescriptorPool(r->device, &dpci, NULL,
                               &state->shader_ds_pool) != VK_SUCCESS) {
        destroy_texture_shader(r, state);
        return;
    }

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = state->shader_ds_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &state->shader_ds_layout,
    };
    if (vkAllocateDescriptorSets(r->device, &dsai, &state->shader_ds) !=
        VK_SUCCESS) {
        destroy_texture_shader(r, state);
        return;
    }

    VkDescriptorImageInfo dii = {
        .sampler = state->shader_sampler,
        .imageView = state->shader_src_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = state->shader_ds,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii,
    };
    vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(TextureShaderPushConstants),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &state->shader_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(r->device, &plci, NULL,
                               &state->shader_pipeline_layout) !=
        VK_SUCCESS) {
        destroy_texture_shader(r, state);
        return;
    }

    /*
     * finalLayout SHADER_READ_ONLY_OPTIMAL means the render pass performs
     * the transition itself, so no explicit barrier is needed after the draw.
     */
    VkAttachmentDescription color_attachment = {
        .format = format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    if (vkCreateRenderPass(r->device, &rpci, NULL,
                           &state->shader_render_pass) != VK_SUCCESS) {
        destroy_texture_shader(r, state);
        return;
    }

    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = state->shader_render_pass,
        .attachmentCount = 1,
        .pAttachments = &state->binding->image_view,
        .width = state->shader_width,
        .height = state->shader_height,
        .layers = 1,
    };
    if (vkCreateFramebuffer(r->device, &fbci, NULL,
                            &state->shader_framebuffer) != VK_SUCCESS) {
        destroy_texture_shader(r, state);
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = state->shader_vs->module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = state->shader_fs->module,
            .pName = "main",
        },
    };

    /* No vertex buffers: the vertex shader builds a triangle from its index. */
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = {
        .width = (float)state->shader_width,
        .height = (float)state->shader_height,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = { state->shader_width, state->shader_height },
    };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading = 1.0f,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .layout = state->shader_pipeline_layout,
        .renderPass = state->shader_render_pass,
        .subpass = 0,
    };

    if (vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &gpci, NULL,
                                  &state->shader_pipeline) != VK_SUCCESS) {
        fprintf(stderr, "nv2a: texture-io: shader pipeline failed for %s\n",
                path);
        destroy_texture_shader(r, state);
        return;
    }

    state->has_shader = true;
    state->shader_hash = hash;

    GStatBuf st;
    state->shader_mtime = (g_stat(path, &st) == 0) ? (int64_t)st.st_mtime : 0;
    state->shader_check_us = INT64_MIN;

    fprintf(stderr, "nv2a: texture-io: loaded shader %s (%ux%u%s)\n", path,
            state->shader_width, state->shader_height,
            has_source ? ", with iChannel0" : ", procedural");
}

/*
 * Re-upload iChannel0 when its animation frame advances, so a .shader paired
 * with a .gif/.webp distorts live frames. Only runs on an actual frame
 * change, so the cost is the animation's rate, not the draw rate.
 */
static void refresh_shader_source(PGRAPHState *pg, XemuTexturePacksVKBindingState *state,
                                  uint64_t hash, int64_t now_us)
{
    if (!state->shader_src_animated ||
        state->shader_src_image == VK_NULL_HANDLE) {
        return;
    }

    int frame = xemu_texture_packs_animated_frame_index(hash, NULL, now_us);
    if (frame < 0 || frame == state->shader_src_frame) {
        return;
    }

    int sw = 0, sh = 0;
    const uint8_t *pixels =
        xemu_texture_packs_animated_frame_pixels(hash, NULL, frame, &sw, &sh);

    if (pixels == NULL || (uint32_t)sw != state->shader_src_width ||
        (uint32_t)sh != state->shader_src_height) {
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    const size_t size = (size_t)sw * sh * 4;

    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation buf_alloc = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo bac = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    };

    if (vmaCreateBuffer(r->allocator, &bci, &bac, &buf, &buf_alloc, NULL) !=
        VK_SUCCESS) {
        return;
    }

    uint8_t *mapped = NULL;
    VK_CHECK(vmaMapMemory(r->allocator, buf_alloc, (void *)&mapped));
    memcpy(mapped, pixels, size);
    vmaFlushAllocation(r->allocator, buf_alloc, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(r->allocator, buf_alloc);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);

    transition_mip_level(cmd, state->shader_src_image, 0,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 1);

    VkBufferImageCopy region = {
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageExtent = { (uint32_t)sw, (uint32_t)sh, 1 },
    };
    vkCmdCopyBufferToImage(cmd, buf, state->shader_src_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition_mip_level(cmd, state->shader_src_image, 0,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1);

    pgraph_vk_end_single_time_commands(pg, cmd);
    vmaDestroyBuffer(r->allocator, buf, buf_alloc);

    state->shader_src_frame = frame;
}

/*
 * Recompile and rebuild the pipeline when the .shader file changes on disk.
 *
 * Only the shader modules and pipeline are rebuilt; the render pass,
 * framebuffer, descriptors, sampler, and source image all stay valid since
 * none of them depend on the shader body. If compilation or pipeline
 * creation fails, everything new is discarded and the previous pipeline
 * keeps running, so a syntax error mid-edit does not blank the texture.
 *
 * vkDeviceWaitIdle before destroying the old pipeline: it may still be
 * referenced by command buffers in flight, and destroying a pipeline in use
 * is undefined behaviour. Reloads are rare (only on file save), so the stall
 * is not a concern.
 */
static void reload_texture_shader_if_changed(PGRAPHState *pg,
                                             XemuTexturePacksVKBindingState *state,
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

    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree char *body = NULL;
    gsize body_len = 0;
    if (!g_file_get_contents(path, &body, &body_len, NULL)) {
        return;
    }

    g_autofree char *fs_src =
        g_strconcat(vk_texture_shader_fs_prologue, body, NULL);

    ShaderModuleInfo *new_fs = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, fs_src);

    if (new_fs == NULL || new_fs->module == VK_NULL_HANDLE) {
        fprintf(stderr,
                "nv2a: texture-io: shader %s failed to compile; keeping "
                "previous version\n", path);
        if (new_fs) {
            pgraph_vk_destroy_shader_module(r, new_fs);
        }
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = state->shader_vs->module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = new_fs->module,
            .pName = "main",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = {
        .width = (float)state->shader_width,
        .height = (float)state->shader_height,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = { state->shader_width, state->shader_height },
    };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading = 1.0f,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .layout = state->shader_pipeline_layout,
        .renderPass = state->shader_render_pass,
        .subpass = 0,
    };

    VkPipeline new_pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &gpci, NULL,
                                  &new_pipeline) != VK_SUCCESS) {
        fprintf(stderr,
                "nv2a: texture-io: pipeline rebuild failed for %s; keeping "
                "previous version\n", path);
        pgraph_vk_destroy_shader_module(r, new_fs);
        return;
    }

    /* New pipeline is good; retire the old one once the GPU is done with it. */
    vkDeviceWaitIdle(r->device);
    vkDestroyPipeline(r->device, state->shader_pipeline, NULL);
    pgraph_vk_destroy_shader_module(r, state->shader_fs);

    state->shader_pipeline = new_pipeline;
    state->shader_fs = new_fs;
    state->shader_last_us = INT64_MIN;

    fprintf(stderr, "nv2a: texture-io: reloaded shader %s\n", path);
}

/* Draw one frame of a binding's shader. Throttled to ~60Hz. */
static void render_texture_shader(PGRAPHState *pg, XemuTexturePacksVKBindingState *state,
                                  int64_t now_us)
{
    if (!state->has_shader) {
        return;
    }

    if (state->shader_last_us != INT64_MIN &&
        now_us >= state->shader_last_us &&
        (now_us - state->shader_last_us) < TEXTURE_SHADER_INTERVAL_US) {
        return;
    }
    state->shader_last_us = now_us;

    /* Live iChannel0 before the shader samples it. */
    refresh_shader_source(pg, state, state->binding->hash, now_us);

    PGRAPHVkState *r = pg->vk_renderer_state;
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = state->shader_render_pass,
        .framebuffer = state->shader_framebuffer,
        .renderArea.extent = { state->shader_width, state->shader_height },
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      state->shader_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            state->shader_pipeline_layout, 0, 1,
                            &state->shader_ds, 0, NULL);

    TextureShaderPushConstants pc = {
        .iTime = (float)(now_us / 1000) / 1000.0f,
        .iResolution = { (float)state->shader_width,
                         (float)state->shader_height },
        .iFrame = state->shader_frame,
        .iHasChannel0 = state->shader_src_view != VK_NULL_HANDLE,
    };
    vkCmdPushConstants(cmd, state->shader_pipeline_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    /* The render pass left the image ready to sample. */
    state->binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    state->shader_frame++;
}


void xemu_texture_packs_vk_plan(XemuTexturePacksVKPlan *plan,
                                uint64_t hash,
                                int dimensionality,
                                bool cubemap,
                                uint32_t guest_width,
                                uint32_t guest_height,
                                bool surface_to_texture)
{
    memset(plan, 0, sizeof(*plan));

    if (!xemu_texture_packs_replace_enabled() || hash == 0 ||
        surface_to_texture || dimensionality != 2) {
        return;
    }

    int rw = 0, rh = 0;
    if (cubemap) {
        if (xemu_texture_packs_has_all_cubemap_faces(hash, &rw, &rh)) {
            plan->replaced = true;
            plan->width = (uint32_t)rw;
            plan->height = (uint32_t)rh;
        }
    } else if (xemu_texture_packs_get_replacement_size(hash, &rw, &rh)) {
        plan->replaced = true;
        plan->width = (uint32_t)rw;
        plan->height = (uint32_t)rh;
    }

    if (!cubemap && xemu_texture_packs_get_shader_path(hash, NULL) != NULL) {
        plan->has_shader = true;
        if (!plan->replaced) {
            plan->replaced = true;
            plan->width = guest_width;
            plan->height = guest_height;
        }
    }

    if (plan->replaced && !cubemap) {
        plan->is_animated =
            xemu_texture_packs_replacement_is_animated(hash, NULL);
        if (plan->is_animated) {
            plan->anim_frame = xemu_texture_packs_animated_frame_index(
                hash, NULL, xemu_texture_packs_anim_now_us());
            if (plan->anim_frame < 0) {
                plan->anim_frame = 0;
            }
        }
    }

    if (plan->replaced) {
        plan->mip_levels = plan->has_shader ? 1 :
            replacement_mip_levels(plan->width, plan->height);
    }
}

bool xemu_texture_packs_vk_binding_created(PGRAPHState *pg,
                                           TextureBinding *binding,
                                           const XemuTexturePacksVKPlan *plan,
                                           VkFormat image_format)
{
    if (plan == NULL || !plan->replaced) {
        return false;
    }

    XemuTexturePacksVKBindingState *state = vk_state_create(binding);
    state->replaced = true;
    state->replacement_width = plan->width;
    state->replacement_height = plan->height;
    state->is_animated = plan->is_animated;
    state->anim_frame = plan->anim_frame;

    if (plan->has_shader) {
        setup_texture_shader(pg, state, binding->hash,
                             plan->width, plan->height, image_format);
        if (state->has_shader) {
            render_texture_shader(pg, state, xemu_texture_packs_anim_now_us());
        }
    }

    return state->has_shader;
}

void xemu_texture_packs_vk_binding_destroy(PGRAPHVkState *r,
                                           TextureBinding *binding)
{
    XemuTexturePacksVKBindingState *state = vk_state_lookup(binding);
    if (state == NULL) {
        return;
    }

    if (state->has_shader || state->shader_render_pass != VK_NULL_HANDLE ||
        state->shader_pipeline != VK_NULL_HANDLE || state->shader_vs != NULL ||
        state->shader_fs != NULL || state->shader_src_image != VK_NULL_HANDLE) {
        destroy_texture_shader(r, state);
    }

    g_hash_table_remove(vk_binding_states, binding);
    g_free(state);
    if (g_hash_table_size(vk_binding_states) == 0) {
        g_hash_table_destroy(vk_binding_states);
        vk_binding_states = NULL;
    }
}

bool xemu_texture_packs_vk_upload_if_replaced(PGRAPHState *pg,
                                              TextureBinding *binding)
{
    XemuTexturePacksVKBindingState *state = vk_state_lookup(binding);
    if (state == NULL || !state->replaced) {
        return false;
    }
    upload_replacement_image(pg, state);
    return true;
}

static void vk_build_dump_variant(char *out, size_t out_size,
                                  bool cubemap, int layer, int level)
{
    const char *face = cubemap ? xemu_texture_packs_cubemap_face_name(layer) : NULL;
    if (face && level > 0) {
        snprintf(out, out_size, "%s_mip%d", face, level);
    } else if (face) {
        snprintf(out, out_size, "%s", face);
    } else if (level > 0) {
        snprintf(out, out_size, "mip%d", level);
    } else {
        out[0] = '\0';
    }
}

int xemu_texture_packs_vk_dump_level_count(int guest_levels)
{
    if (!xemu_texture_packs_dump_enabled() || guest_levels <= 0) {
        return 0;
    }
    return xemu_texture_packs_dump_mipmaps() ? guest_levels : 1;
}

void xemu_texture_packs_vk_maybe_dump_guest32(uint64_t hash,
                                              bool cubemap,
                                              int layer,
                                              int level,
                                              unsigned int width,
                                              unsigned int height,
                                              uint32_t guest_color_format,
                                              const uint8_t *data)
{
    if (hash == 0 || data == NULL ||
        !xemu_texture_packs_should_dump_level(level)) {
        return;
    }

    char variant[32];
    vk_build_dump_variant(variant, sizeof(variant), cubemap, layer, level);
    xemu_texture_packs_dump_guest32_variant(
        hash, variant[0] ? variant : NULL, width, height, width * 4,
        guest_color_format, data);
}

#define TEXTURE_ANIM_REFRESH_INTERVAL_US 4000

void xemu_texture_packs_vk_refresh_dynamic(PGRAPHState *pg)
{
    if (!xemu_texture_packs_dynamic_enabled() || vk_binding_states == NULL ||
        g_hash_table_size(vk_binding_states) == 0) {
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
    g_hash_table_iter_init(&iter, vk_binding_states);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        XemuTexturePacksVKBindingState *state = value;
        if (state->has_shader) {
            reload_texture_shader_if_changed(pg, state, now_us);
            render_texture_shader(pg, state, now_us);
            continue;
        }
        if (!state->replaced || !state->is_animated) {
            continue;
        }
        int frame = xemu_texture_packs_animated_frame_index(
            state->binding->hash, NULL, now_us);
        if (frame < 0 || frame == state->anim_frame) {
            continue;
        }
        state->anim_frame = frame;
        upload_replacement_image(pg, state);
    }
}

bool xemu_texture_packs_vk_binding_is_replaced(TextureBinding *binding)
{
    XemuTexturePacksVKBindingState *state = vk_state_lookup(binding);
    return state != NULL && state->replaced;
}

bool xemu_texture_packs_vk_binding_has_shader(TextureBinding *binding)
{
    XemuTexturePacksVKBindingState *state = vk_state_lookup(binding);
    return state != NULL && state->has_shader;
}

void xemu_texture_packs_vk_binding_dimensions(TextureBinding *binding,
                                              uint32_t *width,
                                              uint32_t *height)
{
    XemuTexturePacksVKBindingState *state = vk_state_lookup(binding);
    if (width != NULL) {
        *width = state != NULL ? state->replacement_width : 0;
    }
    if (height != NULL) {
        *height = state != NULL ? state->replacement_height : 0;
    }
}

uint32_t xemu_texture_packs_vk_binding_mip_levels(TextureBinding *binding)
{
    XemuTexturePacksVKBindingState *state = vk_state_lookup(binding);
    if (state == NULL || !state->replaced) {
        return 0;
    }
    return state->has_shader ? 1 :
        replacement_mip_levels(state->replacement_width,
                               state->replacement_height);
}

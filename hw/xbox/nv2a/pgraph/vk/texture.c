/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
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

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "renderer.h"
#include <glib/gstdio.h>

#include "../gl/texture-io.h"
#include "ui/xemu-settings.h"

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode);

static const VkImageType dimensionality_to_vk_image_type[] = {
    0,
    VK_IMAGE_TYPE_1D,
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_TYPE_3D,
};
static const VkImageViewType dimensionality_to_vk_image_view_type[] = {
    0,
    VK_IMAGE_VIEW_TYPE_1D,
    VK_IMAGE_VIEW_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_3D,
};

static VkSamplerAddressMode lookup_texture_address_mode(int idx)
{
    assert(0 < idx && idx < ARRAY_SIZE(pgraph_texture_addr_vk_map));
    return pgraph_texture_addr_vk_map[idx];
}

// FIXME: Move to common
// FIXME: We can shrink the size of this structure
// FIXME: Use simple allocator
typedef struct TextureLevel {
    unsigned int width, height, depth;
    hwaddr vram_addr;
    void *decoded_data;
    size_t decoded_size;
} TextureLevel;

typedef struct TextureLayer {
    TextureLevel levels[16];
} TextureLayer;

typedef struct TextureLayout {
    TextureLayer layers[6];
} TextureLayout;

// FIXME: Move to common
static enum S3TC_DECOMPRESS_FORMAT kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(!"Invalid texture color format");
    }
}

// FIXME: Move to common
static void memcpy_image(void *dst, void *src, int min_stride, int dst_stride, int src_stride, int height)
{
    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t *src_ptr = (uint8_t *)src;

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, min_stride);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
}

// FIXME: Move to common
static size_t get_cubemap_layer_size(PGRAPHState *pg, TextureShape s)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int block_size;

    unsigned int w = s.width, h = s.height;
    size_t length = 0;

    if (!f.linear && s.border) {
        w = MAX(16, w * 2);
        h = MAX(16, h * 2);
    }

    if (is_compressed) {
        block_size =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                8 :
                16;
    }

    for (int level = 0; level < s.levels; level++) {
        if (is_compressed) {
            length += w / 4 * h / 4 * block_size;
        } else {
            length += w * h * f.bytes_per_pixel;
        }

        w /= 2;
        h /= 2;
    }

    return ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

// FIXME: Move to common
// FIXME: More refactoring
// FIXME: Possible parallelization of decoding
// FIXME: Bounds checking
static TextureLayout *get_texture_layout(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    TextureShape s = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];

    NV2A_VK_DGROUP_BEGIN("Texture %d: cubemap=%d, dimensionality=%d, color_format=0x%x, levels=%d, width=%d, height=%d, depth=%d border=%d, min_mipmap_level=%d, max_mipmap_level=%d, pitch=%d",
        texture_idx,
        s.cubemap,
        s.dimensionality,
        s.color_format,
        s.levels,
        s.width,
        s.height,
        s.depth,
        s.border,
        s.min_mipmap_level,
        s.max_mipmap_level,
        s.pitch
        );

    // Sanity checks on below assumptions
    if (f.linear) {
        assert(s.dimensionality == 2);
    }
    if (s.cubemap) {
        assert(s.dimensionality == 2);
        assert(!f.linear);
    }
    assert(s.dimensionality > 1);

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    void *texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset;

    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);
    void *palette_data_ptr = (char *)d->vram_ptr + texture_palette_vram_offset;

    unsigned int adjusted_width = s.width, adjusted_height = s.height,
                 adjusted_pitch = s.pitch, adjusted_depth = s.depth;

    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    TextureLayout *layout = g_malloc0(sizeof(TextureLayout));

    if (f.linear) {
        assert(s.pitch % f.bytes_per_pixel == 0 && "Can't handle strides unaligned to pixels");

        size_t converted_size;
        uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data_ptr, palette_data_ptr, adjusted_width,
            adjusted_height, 1, adjusted_pitch, 0, &converted_size);

        if (!converted) {
            int dst_stride = adjusted_width * f.bytes_per_pixel;
            assert(adjusted_width <= s.width);
            converted_size = dst_stride * adjusted_height;
            converted = g_malloc(converted_size);
            memcpy_image(converted, texture_data_ptr, adjusted_width * f.bytes_per_pixel, dst_stride,
                         adjusted_pitch, adjusted_height);
        }

        assert(s.levels == 1);
        layout->layers[0].levels[0] = (TextureLevel){
            .width = adjusted_width,
            .height = adjusted_height,
            .depth = 1,
            .decoded_size = converted_size,
            .decoded_data = converted,
        };

        NV2A_VK_DGROUP_END();
        return layout;
    }

    bool is_compressed = pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size = 0;
    if (is_compressed) {
        bool is_dxt1 =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        block_size = is_dxt1 ? 8 : 16;
    }

    if (s.dimensionality == 2) {
        hwaddr layer_size = s.cubemap ? get_cubemap_layer_size(pg, s) : 0;
        const int num_layers = s.cubemap ? 6 : 1;
        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset +
                               layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                NV2A_VK_DPRINTF("Layer %d Level %d @ %x", layer, level, (int)((char*)texture_data_ptr - (char*)d->vram_ptr));

                width = MAX(width, 1);
                height = MAX(height, 1);
                if (is_compressed) {
                    // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                    unsigned int tex_width = width, tex_height = height;
                    unsigned int physical_width = (width + 3) & ~3,
                                 physical_height = (height + 3) & ~3;

                    size_t converted_size = width * height * 4;
                    uint8_t *converted = s3tc_decompress_2d(
                        kelvin_format_to_s3tc_format(s.color_format),
                        texture_data_ptr, width, height);
                    assert(converted);

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.

                        // glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
                        // glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
                        tex_width = s.width;
                        tex_height = s.height;
                        // if (physical_width == width) {
                        //     glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        // }

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr +=
                        physical_width / 4 * physical_height / 4 * block_size;
                } else {
                    unsigned int pitch = width * f.bytes_per_pixel;
                    unsigned int tex_width = width, tex_height = height;

                    size_t converted_size = height * pitch;
                    uint8_t *unswizzled = (uint8_t*)g_malloc(height * pitch);
                    unswizzle_rect(texture_data_ptr, width, height,
                                   unswizzled, pitch, f.bytes_per_pixel);

                    uint8_t *converted = pgraph_convert_texture_data(
                        s, unswizzled, palette_data_ptr, width, height, 1,
                        pitch, 0, &converted_size);

                    if (converted) {
                        g_free(unswizzled);
                    } else {
                        converted = unswizzled;
                    }

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.
                        // glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        tex_width = s.width;
                        tex_height = s.height;
                        // pixel_data += 4 * f.bytes_per_pixel + 4 * pitch;

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr += width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }
    } else if (s.dimensionality == 3) {
        assert(!f.linear);
        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;

        for (int level = 0; level < s.levels; level++) {
            if (is_compressed) {
                width = MAX(width, 1);
                height = MAX(height, 1);
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                depth = MAX(depth, 1);

                size_t converted_size = width * height * depth * 4;
                uint8_t *converted = s3tc_decompress_3d(
                    kelvin_format_to_s3tc_format(s.color_format),
                    texture_data_ptr, width, height, depth);
                assert(converted);

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += physical_width / 4 * physical_height / 4 * depth * block_size;
            } else {
                width = MAX(width, 1);
                height = MAX(height, 1);
                depth = MAX(depth, 1);

                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;

                size_t unswizzled_size = slice_pitch * depth;
                uint8_t *unswizzled = g_malloc(unswizzled_size);
                unswizzle_box(texture_data_ptr, width, height, depth,
                              unswizzled, row_pitch, slice_pitch,
                              f.bytes_per_pixel);

                size_t converted_size;
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data_ptr, width, height, depth,
                    row_pitch, slice_pitch, &converted_size);

                if (converted) {
                    g_free(unswizzled);
                } else {
                    converted = unswizzled;
                    converted_size = unswizzled_size;
                }

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }

    NV2A_VK_DGROUP_END();
    return layout;
}

struct pgraph_texture_possibly_dirty_struct {
    hwaddr addr, end;
};

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node, void *opaque)
{
    struct pgraph_texture_possibly_dirty_struct *test = opaque;

    TextureBinding *tnode = container_of(node, TextureBinding, node);
    if (tnode->possibly_dirty) {
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

void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    struct pgraph_texture_possibly_dirty_struct test = {
        .addr = addr,
        .end = end,
    };

    lru_visit_active(&d->pgraph.vk_renderer_state->texture_cache,
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
        pgraph_vk_mark_textures_possibly_dirty(d, texture_vram_offset, length);
    }
    if (palette_length && check_texture_dirty(d, palette_vram_offset,
                                                     palette_length)) {
        possibly_dirty = true;
        pgraph_vk_mark_textures_possibly_dirty(d, palette_vram_offset,
                                            palette_length);
    }
    return possibly_dirty;
}

// FIXME: Make sure we update sampler when data matches. Should we add filtering
// options to the textureshape?
/* Full mip chain for a replacement of the given dimensions. */
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
static void upload_replacement_image(PGRAPHState *pg, TextureBinding *binding)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const bool cubemap = binding->key.state.cubemap;
    const int num_layers = cubemap ? 6 : 1;

    const int w = (int)binding->replacement_width;
    const int h = (int)binding->replacement_height;
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
            cubemap ? nv2a_texture_io_cubemap_face_name(i) : NULL;

        if (binding->is_animated) {
            /*
             * Decoded frames are already in memory; take the one due now
             * instead of reloading (and re-decoding frame 0) from disk.
             */
            const uint8_t *frame_pixels =
                nv2a_texture_io_animated_frame_pixels(
                    binding->hash, variant, binding->anim_frame, &lw, &lh);
            layer_pixels[i] = (uint8_t *)frame_pixels;
            owns_pixels[i] = false;
        } else {
            layer_pixels[i] = nv2a_texture_io_load_replacement_rgba_variant(
                binding->hash, variant, &lw, &lh);
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
                nv2a_texture_io_free_pixels(layer_pixels[i]);
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
                    size, binding->hash);
            for (int i = 0; i < num_layers; i++) {
                if (owns_pixels[i]) {
                    nv2a_texture_io_free_pixels(layer_pixels[i]);
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
            nv2a_texture_io_free_pixels(layer_pixels[i]);
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
    transition_mip_level(cmd, binding->image, 0, binding->current_layout,
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
    vkCmdCopyBufferToImage(cmd, src_buffer, binding->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, num_layers,
                           regions);

    /* Successively blit each level down by half. */
    int32_t mip_w = w, mip_h = h;

    for (uint32_t level = 1; level < mip_levels; level++) {
        int32_t next_w = mip_w > 1 ? mip_w / 2 : 1;
        int32_t next_h = mip_h > 1 ? mip_h / 2 : 1;

        /* Source level: DST -> SRC */
        transition_mip_level(cmd, binding->image, level - 1,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         num_layers);

        /* Destination level: UNDEFINED -> DST */
        transition_mip_level(cmd, binding->image, level,
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

        vkCmdBlitImage(cmd, binding->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, binding->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);

        /* Source level is finished: SRC -> shader read */
        transition_mip_level(cmd, binding->image, level - 1,
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
    transition_mip_level(cmd, binding->image, mip_levels - 1,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         num_layers);

    binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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

static void destroy_texture_shader(PGRAPHVkState *r, TextureBinding *binding)
{
    if (binding->shader_framebuffer) {
        vkDestroyFramebuffer(r->device, binding->shader_framebuffer, NULL);
    }
    if (binding->shader_pipeline) {
        vkDestroyPipeline(r->device, binding->shader_pipeline, NULL);
    }
    if (binding->shader_pipeline_layout) {
        vkDestroyPipelineLayout(r->device, binding->shader_pipeline_layout,
                                NULL);
    }
    if (binding->shader_render_pass) {
        vkDestroyRenderPass(r->device, binding->shader_render_pass, NULL);
    }
    if (binding->shader_ds_pool) {
        vkDestroyDescriptorPool(r->device, binding->shader_ds_pool, NULL);
    }
    if (binding->shader_ds_layout) {
        vkDestroyDescriptorSetLayout(r->device, binding->shader_ds_layout,
                                     NULL);
    }
    if (binding->shader_sampler) {
        vkDestroySampler(r->device, binding->shader_sampler, NULL);
    }
    if (binding->shader_src_view) {
        vkDestroyImageView(r->device, binding->shader_src_view, NULL);
    }
    if (binding->shader_src_image) {
        vmaDestroyImage(r->allocator, binding->shader_src_image,
                        binding->shader_src_alloc);
    }
    if (binding->shader_vs) {
        pgraph_vk_destroy_shader_module(r, binding->shader_vs);
    }
    if (binding->shader_fs) {
        pgraph_vk_destroy_shader_module(r, binding->shader_fs);
    }

    binding->has_shader = false;
    binding->shader_framebuffer = VK_NULL_HANDLE;
    binding->shader_pipeline = VK_NULL_HANDLE;
    binding->shader_pipeline_layout = VK_NULL_HANDLE;
    binding->shader_render_pass = VK_NULL_HANDLE;
    binding->shader_ds_pool = VK_NULL_HANDLE;
    binding->shader_ds_layout = VK_NULL_HANDLE;
    binding->shader_sampler = VK_NULL_HANDLE;
    binding->shader_src_view = VK_NULL_HANDLE;
    binding->shader_src_image = VK_NULL_HANDLE;
    binding->shader_vs = NULL;
    binding->shader_fs = NULL;
}

/*
 * Create and populate the iChannel0 source image.
 *
 * A 1x1 transparent image is created when the hash has no image replacement,
 * so the descriptor set always has something valid bound; shaders gate on
 * iHasChannel0 rather than on the descriptor being absent.
 */
static bool create_shader_source_image(PGRAPHState *pg,
                                       TextureBinding *binding,
                                       uint64_t hash, bool *out_has_source)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int sw = 0, sh = 0;
    uint8_t *pixels = NULL;
    const uint8_t *src = NULL;
    const uint8_t transparent[4] = { 0, 0, 0, 0 };

    bool animated = nv2a_texture_io_replacement_is_animated(hash, NULL);

    if (animated) {
        /*
         * Borrowed from the decoded frame cache; not freed here. Also the
         * only path that works for WebP, which stbi_load cannot read.
         */
        src = nv2a_texture_io_animated_frame_pixels(hash, NULL, 0, &sw, &sh);
    } else {
        pixels = nv2a_texture_io_load_replacement_rgba(hash, &sw, &sh);
        src = pixels;
    }

    binding->shader_src_animated = animated;
    binding->shader_src_frame = 0;

    if (src == NULL || sw <= 0 || sh <= 0) {
        sw = 1;
        sh = 1;
        src = transparent;
        binding->shader_src_animated = false;
        *out_has_source = false;
    } else {
        *out_has_source = true;
    }

    binding->shader_src_width = (uint32_t)sw;
    binding->shader_src_height = (uint32_t)sh;

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

    if (vmaCreateImage(r->allocator, &ici, &aci, &binding->shader_src_image,
                       &binding->shader_src_alloc, NULL) != VK_SUCCESS) {
        if (pixels) {
            nv2a_texture_io_free_pixels(pixels);
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
            nv2a_texture_io_free_pixels(pixels);
        }
        return false;
    }

    uint8_t *mapped = NULL;
    VK_CHECK(vmaMapMemory(r->allocator, buf_alloc, (void *)&mapped));
    memcpy(mapped, src, size);
    vmaFlushAllocation(r->allocator, buf_alloc, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(r->allocator, buf_alloc);

    if (pixels) {
        nv2a_texture_io_free_pixels(pixels);
    }

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);

    transition_mip_level(cmd, binding->shader_src_image, 0,
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
    vkCmdCopyBufferToImage(cmd, buf, binding->shader_src_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition_mip_level(cmd, binding->shader_src_image, 0,
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
        .image = binding->shader_src_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };

    return vkCreateImageView(r->device, &ivci, NULL,
                             &binding->shader_src_view) == VK_SUCCESS;
}

/*
 * Build everything needed to render a binding's shader. Called once, after
 * the image and view exist. Any failure tears down cleanly and leaves
 * has_shader false, so a broken shader degrades to a normal texture rather
 * than breaking the frame.
 */
static void setup_texture_shader(PGRAPHState *pg, TextureBinding *binding,
                                 uint64_t hash, uint32_t width,
                                 uint32_t height, VkFormat format)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    const char *path = nv2a_texture_io_get_shader_path(hash, NULL);
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

    binding->shader_width = CLAMP(width, 1, TEXTURE_SHADER_MAX_SIZE);
    binding->shader_height = CLAMP(height, 1, TEXTURE_SHADER_MAX_SIZE);
    binding->shader_frame = 0;
    binding->shader_last_us = INT64_MIN;

    binding->shader_vs = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, vk_texture_shader_vs_src);
    binding->shader_fs = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, fs_src);

    if (binding->shader_vs == NULL || binding->shader_fs == NULL ||
        binding->shader_vs->module == VK_NULL_HANDLE ||
        binding->shader_fs->module == VK_NULL_HANDLE) {
        fprintf(stderr, "nv2a: texture-io: shader %s failed to compile\n",
                path);
        destroy_texture_shader(r, binding);
        return;
    }

    bool has_source = false;
    if (!create_shader_source_image(pg, binding, hash, &has_source)) {
        fprintf(stderr,
                "nv2a: texture-io: could not create shader source image "
                "for %s\n", path);
        destroy_texture_shader(r, binding);
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
    if (vkCreateSampler(r->device, &sci, NULL, &binding->shader_sampler) !=
        VK_SUCCESS) {
        destroy_texture_shader(r, binding);
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
                                    &binding->shader_ds_layout) !=
        VK_SUCCESS) {
        destroy_texture_shader(r, binding);
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
                               &binding->shader_ds_pool) != VK_SUCCESS) {
        destroy_texture_shader(r, binding);
        return;
    }

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = binding->shader_ds_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &binding->shader_ds_layout,
    };
    if (vkAllocateDescriptorSets(r->device, &dsai, &binding->shader_ds) !=
        VK_SUCCESS) {
        destroy_texture_shader(r, binding);
        return;
    }

    VkDescriptorImageInfo dii = {
        .sampler = binding->shader_sampler,
        .imageView = binding->shader_src_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = binding->shader_ds,
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
        .pSetLayouts = &binding->shader_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(r->device, &plci, NULL,
                               &binding->shader_pipeline_layout) !=
        VK_SUCCESS) {
        destroy_texture_shader(r, binding);
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
                           &binding->shader_render_pass) != VK_SUCCESS) {
        destroy_texture_shader(r, binding);
        return;
    }

    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = binding->shader_render_pass,
        .attachmentCount = 1,
        .pAttachments = &binding->image_view,
        .width = binding->shader_width,
        .height = binding->shader_height,
        .layers = 1,
    };
    if (vkCreateFramebuffer(r->device, &fbci, NULL,
                            &binding->shader_framebuffer) != VK_SUCCESS) {
        destroy_texture_shader(r, binding);
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = binding->shader_vs->module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = binding->shader_fs->module,
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
        .width = (float)binding->shader_width,
        .height = (float)binding->shader_height,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = { binding->shader_width, binding->shader_height },
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
        .layout = binding->shader_pipeline_layout,
        .renderPass = binding->shader_render_pass,
        .subpass = 0,
    };

    if (vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &gpci, NULL,
                                  &binding->shader_pipeline) != VK_SUCCESS) {
        fprintf(stderr, "nv2a: texture-io: shader pipeline failed for %s\n",
                path);
        destroy_texture_shader(r, binding);
        return;
    }

    binding->has_shader = true;
    binding->shader_hash = hash;

    GStatBuf st;
    binding->shader_mtime = (g_stat(path, &st) == 0) ? (int64_t)st.st_mtime : 0;
    binding->shader_check_us = INT64_MIN;

    fprintf(stderr, "nv2a: texture-io: loaded shader %s (%ux%u%s)\n", path,
            binding->shader_width, binding->shader_height,
            has_source ? ", with iChannel0" : ", procedural");
}

/*
 * Re-upload iChannel0 when its animation frame advances, so a .shader paired
 * with a .gif/.webp distorts live frames. Only runs on an actual frame
 * change, so the cost is the animation's rate, not the draw rate.
 */
static void refresh_shader_source(PGRAPHState *pg, TextureBinding *binding,
                                  uint64_t hash, int64_t now_us)
{
    if (!binding->shader_src_animated ||
        binding->shader_src_image == VK_NULL_HANDLE) {
        return;
    }

    int frame = nv2a_texture_io_animated_frame_index(hash, NULL, now_us);
    if (frame < 0 || frame == binding->shader_src_frame) {
        return;
    }

    int sw = 0, sh = 0;
    const uint8_t *pixels =
        nv2a_texture_io_animated_frame_pixels(hash, NULL, frame, &sw, &sh);

    if (pixels == NULL || (uint32_t)sw != binding->shader_src_width ||
        (uint32_t)sh != binding->shader_src_height) {
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

    transition_mip_level(cmd, binding->shader_src_image, 0,
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
    vkCmdCopyBufferToImage(cmd, buf, binding->shader_src_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition_mip_level(cmd, binding->shader_src_image, 0,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1);

    pgraph_vk_end_single_time_commands(pg, cmd);
    vmaDestroyBuffer(r->allocator, buf, buf_alloc);

    binding->shader_src_frame = frame;
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
                                             TextureBinding *binding,
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
            .module = binding->shader_vs->module,
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
        .width = (float)binding->shader_width,
        .height = (float)binding->shader_height,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = { binding->shader_width, binding->shader_height },
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
        .layout = binding->shader_pipeline_layout,
        .renderPass = binding->shader_render_pass,
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
    vkDestroyPipeline(r->device, binding->shader_pipeline, NULL);
    pgraph_vk_destroy_shader_module(r, binding->shader_fs);

    binding->shader_pipeline = new_pipeline;
    binding->shader_fs = new_fs;
    binding->shader_last_us = INT64_MIN;

    fprintf(stderr, "nv2a: texture-io: reloaded shader %s\n", path);
}

/* Draw one frame of a binding's shader. Throttled to ~60Hz. */
static void render_texture_shader(PGRAPHState *pg, TextureBinding *binding,
                                  int64_t now_us)
{
    if (!binding->has_shader) {
        return;
    }

    if (binding->shader_last_us != INT64_MIN &&
        now_us >= binding->shader_last_us &&
        (now_us - binding->shader_last_us) < TEXTURE_SHADER_INTERVAL_US) {
        return;
    }
    binding->shader_last_us = now_us;

    /* Live iChannel0 before the shader samples it. */
    refresh_shader_source(pg, binding, binding->hash, now_us);

    PGRAPHVkState *r = pg->vk_renderer_state;
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = binding->shader_render_pass,
        .framebuffer = binding->shader_framebuffer,
        .renderArea.extent = { binding->shader_width, binding->shader_height },
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      binding->shader_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            binding->shader_pipeline_layout, 0, 1,
                            &binding->shader_ds, 0, NULL);

    TextureShaderPushConstants pc = {
        .iTime = (float)(now_us / 1000) / 1000.0f,
        .iResolution = { (float)binding->shader_width,
                         (float)binding->shader_height },
        .iFrame = binding->shader_frame,
        .iHasChannel0 = binding->shader_src_view != VK_NULL_HANDLE,
    };
    vkCmdPushConstants(cmd, binding->shader_pipeline_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    /* The render pass left the image ready to sample. */
    binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    binding->shader_frame++;
}

static void upload_texture_image(PGRAPHState *pg, int texture_idx,
                                 TextureBinding *binding)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &binding->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    if (binding->replaced) {
        upload_replacement_image(pg, binding);
        return;
    }

    g_autofree TextureLayout *layout = get_texture_layout(pg, texture_idx);
    const int num_layers = state->cubemap ? 6 : 1;

    /*
     * Texture dump: level 0 of layer 0 only. decoded_size/(w*h) gives the
     * decoded bytes per pixel without needing to interpret the Vulkan
     * format; anything that is not 32bpp is skipped rather than written
     * out incorrectly.
     */
    if (g_config.general.texture_dump_enabled && binding->hash != 0 &&
        !binding->replaced) {
        const int dump_levels =
            g_config.general.texture_dump_mipmaps ? state->levels : 1;

        for (int li = 0; li < num_layers; li++) {
            for (int lv = 0; lv < dump_levels && lv < state->levels; lv++) {
                TextureLevel *tl = &layout->layers[li].levels[lv];

                if (!tl->width || !tl->height || !tl->decoded_data) {
                    continue;
                }

                /*
                 * decoded_size/(w*h) gives decoded bytes per pixel without
                 * interpreting the Vulkan format; anything not 32bpp is
                 * skipped rather than written out incorrectly.
                 */
                size_t px = (size_t)tl->width * tl->height;
                if (!px || (tl->decoded_size / px) != 4) {
                    continue;
                }

                char variant[32];
                const char *face = state->cubemap ?
                    nv2a_texture_io_cubemap_face_name(li) : NULL;

                if (face && lv > 0) {
                    snprintf(variant, sizeof(variant), "%s_mip%d", face, lv);
                } else if (face) {
                    snprintf(variant, sizeof(variant), "%s", face);
                } else if (lv > 0) {
                    snprintf(variant, sizeof(variant), "mip%d", lv);
                } else {
                    variant[0] = '\0';
                }

                nv2a_texture_io_dump_variant(
                    binding->hash, variant[0] ? variant : NULL, tl->width,
                    tl->height, tl->decoded_data);
            }
        }
    }

    // Calculate decoded texture data size
    size_t texture_data_size = 0;
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            size_t size = layer->levels[level_idx].decoded_size;
            assert(size);
            texture_data_size += size;
        }
    }

    assert(texture_data_size <=
           r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));

    int num_regions = num_layers * state->levels;
    g_autofree VkBufferImageCopy *regions =
        g_malloc0_n(num_regions, sizeof(VkBufferImageCopy));

    VkBufferImageCopy *region = regions;
    VkDeviceSize buffer_offset = 0;

    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        NV2A_VK_DPRINTF("Layer %d", layer_idx);
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            TextureLevel *level = &layer->levels[level_idx];
            NV2A_VK_DPRINTF(" - Level %d, w=%d h=%d d=%d @ %08" HWADDR_PRIx,
                            level_idx, level->width, level->height,
                            level->depth, buffer_offset);
            memcpy(mapped_memory_ptr + buffer_offset, level->decoded_data,
                   level->decoded_size);
            *region = (VkBufferImageCopy){
                .bufferOffset = buffer_offset,
                .bufferRowLength = 0, // Tightly packed
                .bufferImageHeight = 0,
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .imageSubresource.mipLevel = level_idx,
                .imageSubresource.baseArrayLayer = layer_idx,
                .imageSubresource.layerCount = 1,
                .imageOffset = (VkOffset3D){ 0, 0, 0 },
                .imageExtent =
                    (VkExtent3D){ level->width, level->height, level->depth },
            };
            buffer_offset += level->decoded_size;
            region++;
        }
    }
    assert(buffer_offset <= r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    // FIXME: Use nondraw. Need to fill and copy tex buffer at once
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_STAGING_SRC].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           binding->image, binding->current_layout,
                           num_regions, regions);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_4);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    // Release decoded texture data
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            g_free(layer->levels[level_idx].decoded_data);
        }
    }
}

static void copy_zeta_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                         TextureBinding *texture)
{
    assert(!surface->color);

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    bool use_compute_to_convert_depth_stencil =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool compute_needs_finish = use_compute_to_convert_depth_stencil &&
                                pgraph_vk_compute_needs_finish(r);
    if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    size_t copied_image_size =
        scaled_width * scaled_height * surface->host_fmt.host_bytes_per_pixel;
    size_t stencil_buffer_offset = 0;
    size_t stencil_buffer_size = 0;

    int num_regions = 0;
    VkBufferImageCopy regions[2];
    regions[num_regions++] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0, // Tightly packed
        .bufferImageHeight = 0, // Tightly packed
        .imageSubresource.aspectMask = surface->color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){0, 0, 0},
        .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        stencil_buffer_offset =
            ROUND_UP(scaled_width * scaled_height * 4,
                     r->device_props.limits.minStorageBufferOffsetAlignment);
        stencil_buffer_size = scaled_width * scaled_height;
        copied_image_size += stencil_buffer_size;

        regions[num_regions++] = (VkBufferImageCopy){
            .bufferOffset = stencil_buffer_offset,
            .bufferRowLength = 0, // Tightly packed
            .bufferImageHeight = 0, // Tightly packed
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.mipLevel = 0,
            .imageSubresource.baseArrayLayer = 0,
            .imageSubresource.layerCount = 1,
            .imageOffset = (VkOffset3D){0, 0, 0},
            .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
        };
    }
    StorageBuffer *dst_storage_buffer = &r->storage_buffers[BUFFER_COMPUTE_DST];
    assert(dst_storage_buffer->buffer_size >= copied_image_size);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    vkCmdCopyImageToBuffer(
        cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_storage_buffer->buffer,
        num_regions, regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkBuffer texture_source_buffer;

    if (use_compute_to_convert_depth_stencil) {
        size_t packed_image_size = scaled_width * scaled_height * 4;

        VkBufferMemoryBarrier pre_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier pre_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_pack_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(
            pg, surface, cmd,
            r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, false);

        VkBufferMemoryBarrier post_pack_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_pack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
            .size = packed_image_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_pack_dst_barrier, 0, NULL);

        texture_source_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;
    } else {
        VkBufferMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = dst_storage_buffer->buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                             1, &barrier, 0, NULL);

        texture_source_buffer = dst_storage_buffer->buffer;
    }

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    regions[0] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
    };
    vkCmdCopyBufferToImage(
        cmd, texture_source_buffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions);

    VkBufferMemoryBarrier post_copy_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = texture_source_buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

// FIXME: Should be able to skip the copy and sample the original surface image
static void copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture)
{
    if (!surface->color) {
        copy_zeta_surface_to_texture(pg, surface, texture);
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkImageCopy region = {
        .srcSubresource.aspectMask = surface->host_fmt.aspect,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = surface->host_fmt.aspect,
        .dstSubresource.layerCount = 1,
        .extent.width = surface->width,
        .extent.height = surface->height,
        .extent.depth = 1,
    };
    pgraph_apply_scaling_factor(pg, &region.extent.width,
                                &region.extent.height);
    vkCmdCopyImage(cmd, surface->image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                   texture->current_layout, 1, &region);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

static unsigned int vk_format_texel_size(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:                return 1;
    case VK_FORMAT_R8G8_UNORM:              return 2;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:   return 2;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:     return 2;
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:   return 2;
    case VK_FORMAT_R16_UNORM:               return 2;
    case VK_FORMAT_R8G8B8_SNORM:            return 3;
    case VK_FORMAT_B8G8R8A8_UNORM:          return 4;
    case VK_FORMAT_R8G8B8A8_UNORM:          return 4;
    case VK_FORMAT_R32_UINT:                return 4;
    default:                                return 0;
    }
}

static bool check_surface_to_texture_compatiblity(const SurfaceBinding *surface,
                                                  const TextureShape *shape)
{
    if ((!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height ||
        shape->cubemap ||
        shape->levels > 1) {
        return false;
    }

    if (!surface->color) {
        return true;
    }

    VkColorFormatInfo tex_vkf = kelvin_color_format_vk_map[shape->color_format];
    return tex_vkf.vk_format &&
           surface->host_fmt.host_bytes_per_pixel == vk_format_texel_size(tex_vkf.vk_format);
}

static void create_dummy_texture(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = 16,
        .extent.height = 16,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkImage texture_image;
    VmaAllocation texture_allocation;

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &texture_image,
                            &texture_allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R },
    };
    VkImageView texture_image_view;
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &texture_image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VkSampler texture_sampler;
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &texture_sampler));

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;
    size_t texture_data_size =
        image_create_info.extent.width * image_create_info.extent.height;

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));
    memset(mapped_memory_ptr, 0xff, texture_data_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, texture_image, VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ image_create_info.extent.width,
                                     image_create_info.extent.height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, texture_image,
                                      VK_FORMAT_R8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd);

    r->dummy_texture = (TextureBinding){
        .key.scale = 1.0,
        .image = texture_image,
        .current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .allocation = texture_allocation,
        .image_view = texture_image_view,
        .sampler = texture_sampler,
    };
}

static void destroy_dummy_texture(PGRAPHVkState *r)
{
    texture_cache_release_node_resources(r, &r->dummy_texture);
}

static void set_texture_label(PGRAPHState *pg, TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
        texture->key.texture_vram_offset, texture->key.state.color_format,
        texture->key.state.width, texture->key.state.height,
        texture->key.state.depth, texture->key.state.levels);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)texture->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, texture->allocation, label);
}

static bool is_linear_filter_supported_for_format(PGRAPHVkState *r,
                                                  int kelvin_format)
{
    return r->texture_format_properties[kelvin_format].optimalTilingFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}

static void create_texture(PGRAPHState *pg, int texture_idx)
{
    NV2A_VK_DGROUP_BEGIN("Creating texture %d", texture_idx);

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx); // FIXME: Check for pad issues
    BasicColorFormatInfo f_basic = kelvin_color_format_info_map[state.color_format];

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    size_t texture_length = pgraph_get_texture_length(pg, &state);
    hwaddr texture_palette_vram_offset = 0;
    size_t texture_palette_data_size = 0;

    uint32_t filter =
        pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    uint32_t address =
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    uint32_t border_color_pack32 =
        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + texture_idx * 4);
    bool is_indexed = (state.color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
    uint32_t max_anisotropy =
        1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + texture_idx*4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = texture_vram_offset;
    key.texture_length = texture_length;
    if (is_indexed) {
        texture_palette_vram_offset =
            pgraph_get_texture_palette_phys_addr_length(
                pg, texture_idx, &texture_palette_data_size);
        key.palette_vram_offset = texture_palette_vram_offset;
        key.palette_length = texture_palette_data_size;
    }
    key.scale = 1;

    // FIXME: Separate sampler from texture
    key.filter = filter;
    key.address = address;
    key.border_color = border_color_pack32;
    key.max_anisotropy = max_anisotropy;

    bool possibly_dirty = false;
    bool possibly_dirty_checked = false;
    bool surface_to_texture = false;

    // Check active surfaces to see if this texture was a render target
    SurfaceBinding *surface = pgraph_vk_surface_get(d, texture_vram_offset);
    if (surface && state.levels == 1) {
        surface_to_texture =
            check_surface_to_texture_compatiblity(surface, &state);

        if (!surface_to_texture && surface->color) {
            trace_nv2a_pgraph_surface_texture_compat_failed(
                surface->shape.color_format,
                state.color_format);
        }

        if (surface_to_texture && surface->upload_pending) {
            pgraph_vk_upload_surface_data(d, surface, false);
        }
    }

    if (!surface_to_texture) {
        // FIXME: Restructure to support rendering surfaces to cubemap faces

        // Writeback any surfaces which this texture may index
        pgraph_vk_download_surfaces_in_range_if_dirty(
            pg, texture_vram_offset, texture_length);
    }

    if (surface_to_texture && pg->surface_scale_factor > 1) {
        key.scale = pg->surface_scale_factor;
    }

    uint64_t key_hash = fast_hash((void*)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    bool binding_found = snode->image != VK_NULL_HANDLE;

    if (binding_found) {
        NV2A_VK_DPRINTF("Cache hit");
        r->texture_bindings[texture_idx] = snode;
        possibly_dirty |= snode->possibly_dirty;
    } else {
        possibly_dirty = true;
    }

    if (!surface_to_texture && !possibly_dirty_checked) {
        possibly_dirty |= check_texture_possibly_dirty(
            d, texture_vram_offset, texture_length, texture_palette_vram_offset,
            texture_palette_data_size);
    }

    // Calculate hash of texture data, if necessary
    void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
    void *palette_data = (char*)d->vram_ptr + texture_palette_vram_offset;

    uint64_t content_hash = 0;
    if (!surface_to_texture && possibly_dirty) {
        content_hash = fast_hash(texture_data, texture_length);
        if (is_indexed) {
            content_hash ^= fast_hash(palette_data, texture_palette_data_size);
        }
    }

    if (binding_found) {
        if (surface_to_texture) {
            // FIXME: Add draw time tracking
            if (surface->draw_time != snode->draw_time) {
                copy_surface_to_texture(pg, surface, snode);
            }
        } else {
            if (possibly_dirty && content_hash != snode->hash) {
                /* Set before upload so the dump hook sees the new hash. */
                snode->hash = content_hash;
                upload_texture_image(pg, texture_idx, snode);
            }
        }

        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");

    memcpy(&snode->key, &key, sizeof(key));
    snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    snode->possibly_dirty = false;
    snode->hash = content_hash;

    /*
     * Texture replacement.
     *
     * The image must be created at the replacement's dimensions, so the
     * lookup has to happen before vmaCreateImage. Cubemaps and volume
     * textures are not supported (each face/slice would need its own file),
     * nor are surface-sourced textures, which have no meaningful data hash.
     */
    snode->replaced = false;
    snode->replacement_width = 0;
    snode->replacement_height = 0;
    snode->is_animated = false;
    snode->anim_frame = 0;
    snode->has_shader = false;
    snode->shader_render_pass = VK_NULL_HANDLE;
    snode->shader_framebuffer = VK_NULL_HANDLE;
    snode->shader_pipeline = VK_NULL_HANDLE;
    snode->shader_pipeline_layout = VK_NULL_HANDLE;
    snode->shader_ds_layout = VK_NULL_HANDLE;
    snode->shader_ds_pool = VK_NULL_HANDLE;
    snode->shader_sampler = VK_NULL_HANDLE;
    snode->shader_src_image = VK_NULL_HANDLE;
    snode->shader_src_view = VK_NULL_HANDLE;
    snode->shader_vs = NULL;
    snode->shader_fs = NULL;
    snode->shader_frame = 0;
    snode->shader_last_us = INT64_MIN;
    snode->shader_src_animated = false;
    snode->shader_src_frame = 0;
    snode->shader_src_width = 0;
    snode->shader_src_height = 0;
    snode->shader_mtime = 0;
    snode->shader_check_us = INT64_MIN;
    snode->shader_hash = 0;

    if (g_config.general.texture_replace_enabled && content_hash != 0 &&
        !surface_to_texture && state.dimensionality == 2) {
        int rw = 0, rh = 0;

        if (state.cubemap) {
            /* All six faces must exist and match in size, or none is used. */
            if (nv2a_texture_io_has_all_cubemap_faces(content_hash, &rw,
                                                      &rh)) {
                snode->replaced = true;
                snode->replacement_width = rw;
                snode->replacement_height = rh;
            }
        } else if (nv2a_texture_io_get_replacement_size(content_hash, &rw,
                                                        &rh)) {
            snode->replaced = true;
            snode->replacement_width = rw;
            snode->replacement_height = rh;
        }

        /*
         * A shader attachment makes this a replaced texture even with no
         * image file: it renders its own content. Sizing follows the image
         * replacement when one exists (that becomes iChannel0), otherwise
         * the guest texture's own size.
         */
        if (!state.cubemap &&
            nv2a_texture_io_get_shader_path(content_hash, NULL) != NULL) {
            snode->has_shader = true;
            if (!snode->replaced) {
                snode->replaced = true;
                snode->replacement_width = state.width;
                snode->replacement_height = state.height;
            }
        }

        /*
         * Animated replacements are only supported for plain 2D textures;
         * a cubemap would need per-face animation timing.
         */
        if (snode->replaced && !state.cubemap) {
            snode->is_animated =
                nv2a_texture_io_replacement_is_animated(content_hash, NULL);
            snode->anim_frame = nv2a_texture_io_animated_frame_index(
                content_hash, NULL, nv2a_texture_io_anim_now_us());
            if (snode->anim_frame < 0) {
                snode->anim_frame = 0;
            }
        }
    }

    /*
     * Replacements are decoded to RGBA8 with identity swizzle, overriding
     * the guest format and its component mapping.
     */
    VkFormat img_format = kelvin_color_format_vk_map[state.color_format].vk_format;
    VkComponentMapping img_components =
        kelvin_color_format_vk_map[state.color_format].component_map;
    if (snode->replaced) {
        img_format = VK_FORMAT_R8G8B8A8_UNORM;
        img_components = (VkComponentMapping){
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
        };
    }

    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state.color_format];
    assert(vkf.vk_format != 0);
    assert(0 < state.dimensionality);
    assert(state.dimensionality < ARRAY_SIZE(dimensionality_to_vk_image_type));
    assert(state.dimensionality <
           ARRAY_SIZE(dimensionality_to_vk_image_view_type));

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimensionality_to_vk_image_type[state.dimensionality],
        .extent.width = snode->replaced ? snode->replacement_width :
                                          state.width,
        .extent.height = snode->replaced ? snode->replacement_height :
                                           state.height,
        .extent.depth = snode->replaced ? 1 : state.depth,
        /*
         * Shader targets keep a single level: the render pass writes level 0
         * and there is no cheap in-pass way to regenerate a chain.
         */
        .mipLevels = snode->has_shader ? 1 :
                     (snode->replaced ?
                         replacement_mip_levels(snode->replacement_width,
                                                snode->replacement_height) :
                         (f_basic.linear ? 1 : state.levels)),
        .arrayLayers = state.cubemap ? 6 : 1,
        .format = img_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT |
                 (snode->replaced ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) |
                 /* Shader targets are rendered into, not just copied to. */
                 (snode->has_shader ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = (state.cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
    };

    if (surface_to_texture) {
        pgraph_apply_scaling_factor(pg, &image_create_info.extent.width,
                                        &image_create_info.extent.height);
    }

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &snode->image,
                            &snode->allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = snode->image,
        .viewType = state.cubemap ?
            VK_IMAGE_VIEW_TYPE_CUBE :
            dimensionality_to_vk_image_view_type[state.dimensionality],
        .format = img_format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = img_components,
    };

    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &snode->image_view));


    void *sampler_next_struct = NULL;

    VkSamplerCustomBorderColorCreateInfoEXT custom_border_color_create_info;
    VkBorderColor vk_border_color;

    bool is_integer_type = img_format == VK_FORMAT_R32_UINT;

    if (r->custom_border_color_extension_enabled) {
        vk_border_color = is_integer_type ? VK_BORDER_COLOR_INT_CUSTOM_EXT :
                                            VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        custom_border_color_create_info =
            (VkSamplerCustomBorderColorCreateInfoEXT){
                .sType =
                    VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                .format = image_view_create_info.format,
                .pNext = sampler_next_struct
            };
        if (is_integer_type) {
            float rgba[4];
            pgraph_argb_pack32_to_rgba_float(border_color_pack32, rgba);
            for (int i = 0; i < 4; i++) {
                custom_border_color_create_info.customBorderColor.uint32[i] =
                    (uint32_t)((double)rgba[i] * (double)0xffffffff);
            }
        } else {
            pgraph_argb_pack32_to_rgba_float(
                border_color_pack32,
                custom_border_color_create_info.customBorderColor.float32);
        }
        sampler_next_struct = &custom_border_color_create_info;
    } else {
        // FIXME: Handle custom color in shader
        if (is_integer_type) {
            vk_border_color = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0x00000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0xff000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        } else {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        }
    }

    if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

    VkFilter vk_min_filter, vk_mag_filter;
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    assert(mag_filter < ARRAY_SIZE(pgraph_texture_mag_filter_vk_map));

    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    assert(min_filter < ARRAY_SIZE(pgraph_texture_min_filter_vk_map));

    if (is_linear_filter_supported_for_format(r, state.color_format)) {
        vk_mag_filter = pgraph_texture_min_filter_vk_map[mag_filter];
        vk_min_filter = pgraph_texture_min_filter_vk_map[min_filter];
    } else {
        vk_mag_filter = vk_min_filter = VK_FILTER_NEAREST;
    }

    bool mipmap_en =
        !f_basic.linear &&
        !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);

    bool mipmap_nearest =
        f_basic.linear || image_create_info.mipLevels == 1 ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

    float lod_bias = pgraph_convert_lod_bias_to_float(
        GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS));
    if (lod_bias > r->device_props.limits.maxSamplerLodBias) {
        lod_bias = r->device_props.limits.maxSamplerLodBias;
    } else if (lod_bias < -r->device_props.limits.maxSamplerLodBias) {
        lod_bias = -r->device_props.limits.maxSamplerLodBias;
    }
    uint32_t sampler_max_anisotropy =
        MIN(r->device_props.limits.maxSamplerAnisotropy, max_anisotropy);

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_mag_filter,
        .minFilter = vk_min_filter,
        .addressModeU = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .addressModeV = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .addressModeW = (state.dimensionality > 2) ? lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)) : 0,
        .anisotropyEnable =
            r->enabled_physical_device_features.samplerAnisotropy &&
            sampler_max_anisotropy > 1,
        .maxAnisotropy = sampler_max_anisotropy,
        .borderColor = vk_border_color,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = mipmap_nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST :
                                       VK_SAMPLER_MIPMAP_MODE_LINEAR,
        /*
         * Shader-generated images have a single level (the render pass has
         * nowhere to write a mip chain), so the guest's LOD range must not
         * be applied -- sampling above level 0 would be out of range.
         */
        .minLod = 0.0,
        .maxLod = snode->has_shader ? 0.0 :
                  (mipmap_en ? MIN(state.max_mipmap_level, state.levels - 1) :
                               0.0),
        .mipLodBias = lod_bias,
        .pNext = sampler_next_struct,
    };

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &snode->sampler));

    set_texture_label(pg, snode);

    r->texture_bindings[texture_idx] = snode;

    if (snode->has_shader) {
        /*
         * The shader owns this image's contents, so the normal upload is
         * skipped entirely; any image replacement becomes iChannel0 inside
         * setup rather than being written here.
         */
        setup_texture_shader(pg, snode, snode->hash, snode->replacement_width,
                             snode->replacement_height, img_format);

        if (snode->has_shader) {
            render_texture_shader(pg, snode, nv2a_texture_io_anim_now_us());
        } else {
            /* Setup failed; fall back to a normal texture. */
            upload_texture_image(pg, texture_idx, snode);
            snode->draw_time = 0;
        }
    } else if (surface_to_texture) {
        copy_surface_to_texture(pg, surface, snode);
    } else {
        upload_texture_image(pg, texture_idx, snode);
        snode->draw_time = 0;
    }

    NV2A_VK_DGROUP_END();
}

static bool check_textures_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!r->texture_bindings[i] || pg->texture_dirty[i]) {
            return true;
        }
    }
    return false;
}

static void update_timestamps(PGRAPHVkState *r)
{
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i]) {
            r->texture_bindings[i]->submit_time = r->submit_count;
        }
    }
}

/*
 * Re-upload any animated replacement whose displayed frame has advanced.
 *
 * bind_textures runs per draw call, so the frame index is checked first and
 * the (expensive) staging upload + mip regeneration only happens on an
 * actual frame change -- roughly 10 times a second for a typical GIF rather
 * than once per draw.
 */
/*
 * PERFORMANCE: bind_textures() is called once per DRAW CALL, not once per
 * frame, and lru_add_free() puts free nodes on the global list too -- so an
 * unguarded walk here costs a full 1024-node linked-list traversal on every
 * draw, whether or not any replacement texture exists. In a draw-heavy title
 * that is millions of cache-hostile pointer chases per frame, paid even with
 * texture replacement switched off. It looks like a game/renderer regression
 * rather than a feature cost, so it is gated twice: nothing can be animated
 * or shader-backed unless replacements are enabled, and the walk itself runs
 * at a fixed 250 Hz rather than per draw -- far above the ~10-30 fps of the
 * source media.
 */
#define TEXTURE_ANIM_REFRESH_INTERVAL_US 4000

static void refresh_animated_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->texture_cache_entries == NULL) {
        return;
    }

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
        TextureBinding *binding = container_of(node, TextureBinding, node);

        if (binding->image == VK_NULL_HANDLE) {
            continue;
        }

        if (binding->has_shader) {
            reload_texture_shader_if_changed(pg, binding, now_us);
            render_texture_shader(pg, binding, now_us);
            continue;
        }

        if (!binding->replaced || !binding->is_animated) {
            continue;
        }

        int frame = nv2a_texture_io_animated_frame_index(binding->hash, NULL,
                                                         now_us);
        if (frame < 0 || frame == binding->anim_frame) {
            continue;
        }

        binding->anim_frame = frame;
        upload_replacement_image(pg, binding);
    }
}

void pgraph_vk_bind_textures(NV2AState *d)
{
    if (nv2a_texture_io_consume_flush_request()) {
        pgraph_vk_texture_cache_flush();
    }

    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    refresh_animated_textures(pg);

    // FIXME: Check for modifications on bind fastpath (CPU hook)
    // FIXME: Mark textures that are sourced from surfaces so we can track them

    r->texture_bindings_changed = false;

    if (!check_textures_dirty(pg)) {
        NV2A_VK_DPRINTF("Not dirty");
        NV2A_VK_DGROUP_END();
        update_timestamps(r);
        return;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            r->texture_bindings[i] = &r->dummy_texture;
            continue;
        }

        create_texture(pg, i);

        pg->texture_dirty[i] = false; // FIXME: Move to renderer?
    }

    r->texture_bindings_changed = true;
    update_timestamps(r);
    NV2A_VK_DGROUP_END();
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);

    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
    snode->image_view = VK_NULL_HANDLE;
    snode->sampler = VK_NULL_HANDLE;
}

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode)
{
    /*
     * Shader resources reference image_view, so tear them down before the
     * view and image below.
     */
    if (snode->has_shader || snode->shader_render_pass) {
        destroy_texture_shader(r, snode);
    }

    vkDestroySampler(r->device, snode->sampler, NULL);
    snode->sampler = VK_NULL_HANDLE;

    vkDestroyImageView(r->device, snode->image_view, NULL);
    snode->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, snode->image, snode->allocation);
    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    // FIXME: Simplify. We don't really need to check bindings


    // Currently bound
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i] == snode) {
            return false;
        }
    }

    // Used in command buffer
    if (r->in_command_buffer && snode->submit_time == r->submit_count) {
        return false;
    }

    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    texture_cache_release_node_resources(r, snode);
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    return memcmp(&snode->key, key, sizeof(TextureKey));
}

static void texture_cache_init(PGRAPHVkState *r)
{
    const size_t texture_cache_size = 1024;
    lru_init(&r->texture_cache);
    /*
     * Zeroed: the animated-texture sweep walks the LRU global list, which
     * includes not-yet-used free nodes, so their replaced/is_animated/image
     * fields must be false/NULL rather than uninitialised garbage.
     */
    r->texture_cache_entries =
        g_malloc0_n(texture_cache_size, sizeof(TextureBinding));
    assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;

    nv2a_texture_io_set_backend(NV2A_TEXTURE_BACKEND_VK);
}

static void texture_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->texture_cache);
    g_free(r->texture_cache_entries);
    r->texture_cache_entries = NULL;
}

void pgraph_vk_trim_texture_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Allow specifying some amount to trim by

    int num_to_evict = r->texture_cache.num_used / 4;
    int num_evicted = 0;

    while (num_to_evict-- && lru_try_evict_one(&r->texture_cache)) {
        num_evicted += 1;
    }

    NV2A_VK_DPRINTF("Evicted %d textures, %d remain", num_evicted, r->texture_cache.num_used);
}

void pgraph_vk_init_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    texture_cache_init(r);
    create_dummy_texture(pg);

    r->texture_format_properties = g_malloc0_n(
        ARRAY_SIZE(kelvin_color_format_vk_map), sizeof(VkFormatProperties));
    for (int i = 0; i < ARRAY_SIZE(kelvin_color_format_vk_map); i++) {
        vkGetPhysicalDeviceFormatProperties(
            r->physical_device, kelvin_color_format_vk_map[i].vk_format,
            &r->texture_format_properties[i]);
    }
}

void pgraph_vk_finalize_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_command_buffer);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }

    destroy_dummy_texture(r);
    texture_cache_finalize(r);

    assert(r->texture_cache.num_used == 0);

    g_free(r->texture_format_properties);
    r->texture_format_properties = NULL;
}

void pgraph_vk_texture_cache_flush(void)
{
    if (g_nv2a == NULL ||
        nv2a_texture_io_get_backend() != NV2A_TEXTURE_BACKEND_VK) {
        return;
    }

    PGRAPHState *pg = &g_nv2a->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r == NULL || r->texture_cache_entries == NULL) {
        return;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }

    lru_flush(&r->texture_cache);
}

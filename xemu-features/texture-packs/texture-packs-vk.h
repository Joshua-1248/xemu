#ifndef XEMU_FEATURES_TEXTURE_PACKS_VK_H
#define XEMU_FEATURES_TEXTURE_PACKS_VK_H
#include "config-host.h"
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
typedef struct PGRAPHState PGRAPHState;
typedef struct TextureBinding TextureBinding;
typedef struct PGRAPHVkState PGRAPHVkState;
typedef struct XemuTexturePacksVKPlan {
    bool replaced;
    bool has_shader;
    bool is_animated;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    int anim_frame;
} XemuTexturePacksVKPlan;
#ifdef CONFIG_XEMU_FEATURE_TEXTURE_PACKS
void xemu_texture_packs_vk_plan(XemuTexturePacksVKPlan *plan, uint64_t hash,
                                int dimensionality, bool cubemap,
                                uint32_t guest_width, uint32_t guest_height,
                                bool surface_to_texture);
bool xemu_texture_packs_vk_binding_created(PGRAPHState *pg, TextureBinding *binding,
                                           const XemuTexturePacksVKPlan *plan,
                                           VkFormat image_format);
void xemu_texture_packs_vk_binding_destroy(PGRAPHVkState *r, TextureBinding *binding);
bool xemu_texture_packs_vk_upload_if_replaced(PGRAPHState *pg, TextureBinding *binding);
int xemu_texture_packs_vk_dump_level_count(int guest_levels);
void xemu_texture_packs_vk_maybe_dump_guest32(uint64_t hash, bool cubemap, int layer,
                                              int level, unsigned int width,
                                              unsigned int height,
                                              uint32_t guest_color_format,
                                              const uint8_t *data);
void xemu_texture_packs_vk_refresh_dynamic(PGRAPHState *pg);
bool xemu_texture_packs_vk_binding_is_replaced(TextureBinding *binding);
bool xemu_texture_packs_vk_binding_has_shader(TextureBinding *binding);
void xemu_texture_packs_vk_binding_dimensions(TextureBinding *binding,
                                              uint32_t *width, uint32_t *height);
uint32_t xemu_texture_packs_vk_binding_mip_levels(TextureBinding *binding);
#else
static inline void xemu_texture_packs_vk_plan(XemuTexturePacksVKPlan *plan, uint64_t hash,
                                              int dimensionality, bool cubemap,
                                              uint32_t guest_width, uint32_t guest_height,
                                              bool surface_to_texture)
{
    (void)hash; (void)dimensionality; (void)cubemap; (void)guest_width;
    (void)guest_height; (void)surface_to_texture;
    if (plan) {
        plan->replaced = false; plan->has_shader = false; plan->is_animated = false;
        plan->width = 0; plan->height = 0; plan->mip_levels = 0; plan->anim_frame = 0;
    }
}
static inline bool xemu_texture_packs_vk_binding_created(PGRAPHState *pg, TextureBinding *binding,
                                                         const XemuTexturePacksVKPlan *plan,
                                                         VkFormat image_format)
{ (void)pg; (void)binding; (void)plan; (void)image_format; return false; }
static inline void xemu_texture_packs_vk_binding_destroy(PGRAPHVkState *r, TextureBinding *binding)
{ (void)r; (void)binding; }
static inline bool xemu_texture_packs_vk_upload_if_replaced(PGRAPHState *pg, TextureBinding *binding)
{ (void)pg; (void)binding; return false; }
static inline int xemu_texture_packs_vk_dump_level_count(int guest_levels) { (void)guest_levels; return 0; }
static inline void xemu_texture_packs_vk_maybe_dump_guest32(uint64_t hash, bool cubemap, int layer,
                                                            int level, unsigned int width,
                                                            unsigned int height,
                                                            uint32_t guest_color_format,
                                                            const uint8_t *data)
{ (void)hash; (void)cubemap; (void)layer; (void)level; (void)width; (void)height; (void)guest_color_format; (void)data; }
static inline void xemu_texture_packs_vk_refresh_dynamic(PGRAPHState *pg) { (void)pg; }
static inline bool xemu_texture_packs_vk_binding_is_replaced(TextureBinding *binding) { (void)binding; return false; }
static inline bool xemu_texture_packs_vk_binding_has_shader(TextureBinding *binding) { (void)binding; return false; }
static inline void xemu_texture_packs_vk_binding_dimensions(TextureBinding *binding,
                                                            uint32_t *width, uint32_t *height)
{ (void)binding; if (width) *width = 0; if (height) *height = 0; }
static inline uint32_t xemu_texture_packs_vk_binding_mip_levels(TextureBinding *binding)
{ (void)binding; return 0; }
#endif
#endif

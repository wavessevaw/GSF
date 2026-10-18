/* SPDX-License-Identifier: GPL-3.0-or-later
 * Experimental Vulkan PRESENT layout compatibility for a virtual source.
 *
 * Vulkan applications must transition swapchain images to PRESENT_SRC_KHR.
 * Plain VkImages are not presentable, so the virtual WSI layer must translate
 * those transitions on registered virtual images before reaching the ICD.
 *
 * This is a narrow, pure transform; hooking CmdPipelineBarrier(2), renderpass
 * creation and tracking image views/framebuffers is NOT implemented here.
 * Never rewrite ordinary downstream (display) WSI images.
 */
#ifndef GFG_VIRTUAL_LAYOUTS_H
#define GFG_VIRTUAL_LAYOUTS_H
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const VkImage *images;
    uint32_t count;
    uint64_t translated_old,translated_new;
} gfg_virtual_layout_map;
static inline int gfg_vlayout_is_virtual(const gfg_virtual_layout_map *m,
                                          VkImage image) {
    if(!m||!image||!m->images)return 0;
    for(uint32_t i=0;i<m->count;++i)
        if(m->images[i]==image)return 1;
    return 0;
}
static inline VkImageLayout gfg_vlayout_rewrite(gfg_virtual_layout_map *m,
                     VkImage image,VkImageLayout layout,int is_new) {
    if(layout!=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
       !gfg_vlayout_is_virtual(m,image))
        return layout;
    if(is_new)++m->translated_new;
    else ++m->translated_old;
    /* The virtual producer never presents the source image. The next
     * operation is a transfer into GSF-owned history, so expose it in
     * TRANSFER_SRC_OPTIMAL instead of illegally handing it to a WSI. */
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
static inline int gfg_vlayout_legacy(gfg_virtual_layout_map *m,
             const VkImageMemoryBarrier *in,VkImageMemoryBarrier *out) {
    if(!in||!out)return 0;
    *out=*in;
    out->oldLayout=gfg_vlayout_rewrite(m,in->image,in->oldLayout,0);
    out->newLayout=gfg_vlayout_rewrite(m,in->image,in->newLayout,1);
    return 1;
}
static inline int gfg_vlayout_sync2(gfg_virtual_layout_map *m,
             const VkImageMemoryBarrier2 *in,VkImageMemoryBarrier2 *out) {
    if(!in||!out)return 0;
    *out=*in;
    out->oldLayout=gfg_vlayout_rewrite(m,in->image,in->oldLayout,0);
    out->newLayout=gfg_vlayout_rewrite(m,in->image,in->newLayout,1);
    return 1;
}
/* Renderpass layout declarations cannot identify virtual images on their
 * own, so do NOT rewrite them globally for a multi-window process.
 * The framebuffers/image views referring to virtual images must be tracked
 * before this function may be applied by an actual Vulkan layer. */
static inline int gfg_vlayout_renderpass_attachment(
        const VkAttachmentDescription *in,VkAttachmentDescription *out,
        int exclusively_virtual) {
    if(!in||!out)return 0;
    *out=*in;
    if(!exclusively_virtual)return 1;
    if(out->initialLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        out->initialLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if(out->finalLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        out->finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return 1;
}
static inline int gfg_vlayout_renderpass2_attachment(
        const VkAttachmentDescription2 *in,VkAttachmentDescription2 *out,
        int exclusively_virtual) {
    if(!in||!out)return 0;
    *out=*in;
    if(!exclusively_virtual)return 1;
    if(out->initialLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        out->initialLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if(out->finalLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        out->finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return 1;
}
#endif

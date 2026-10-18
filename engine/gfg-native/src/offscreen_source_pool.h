/* SPDX-License-Identifier: GPL-3.0-or-later
 * EXPERIMENTAL: GPU-backed source images for a future virtual app swapchain.
 *
 * This module does not intercept vkCreateSwapchainKHR and does not present
 * anything. A virtual source must ultimately be paired with exactly one
 * separate, real compositor-facing swapchain.
 *
 * IMPORTANT: pass next-layer Vulkan dispatch pointers (including physical
 * device dispatch); never call the global loader with borrowed handles.
 */
#ifndef GFG_OFFSCREEN_SOURCE_POOL_H
#define GFG_OFFSCREEN_SOURCE_POOL_H
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <string.h>

enum { GFG_OFFSCREEN_MAX_IMAGES = 8 };

typedef struct {
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements get_image_requirements;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
} gfg_offscreen_dispatch;

typedef struct {
    VkImage images[GFG_OFFSCREEN_MAX_IMAGES];
    VkDeviceMemory memory[GFG_OFFSCREEN_MAX_IMAGES];
    uint32_t count;
    VkFormat format;
    VkExtent2D extent;
    VkImageUsageFlags usage;
} gfg_offscreen_source_pool;

/* Intentionally conservative. Unknown presentation metadata and oldSwapchain
 * need full virtual WSI handling; for now, reject before consuming app sync.
 * The caller MUST forward the original unmodified swapchain create request.
 * Gamescope packed10 / HDR stays real-only until independently qualified. */
static inline const char *gfg_offscreen_unsupported(
        const VkSwapchainCreateInfoKHR *ci) {
    if (!ci || ci->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR)
        return "invalid-create-info";
    if (ci->pNext) return "unknown-pnext";
    if (ci->flags) return "unsupported-swapchain-flags";
    if (ci->oldSwapchain) return "old-swapchain-needs-retirement";
    if (ci->minImageCount == 0 || ci->minImageCount > GFG_OFFSCREEN_MAX_IMAGES)
        return "image-count";
    if (!ci->imageExtent.width || !ci->imageExtent.height)
        return "zero-extent";
    if (ci->imageArrayLayers != 1) return "array-layers";
    if (ci->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE)
        return "concurrent-sharing";
    if (ci->presentMode != VK_PRESENT_MODE_FIFO_KHR)
        return "non-fifo";
    if (ci->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        return "hdr-or-unknown-colorspace";
    if (ci->imageFormat != VK_FORMAT_B8G8R8A8_UNORM &&
        ci->imageFormat != VK_FORMAT_B8G8R8A8_SRGB &&
        ci->imageFormat != VK_FORMAT_R8G8B8A8_UNORM &&
        ci->imageFormat != VK_FORMAT_R8G8B8A8_SRGB)
        return "unsupported-format";
    if (!(ci->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return "not-color-attachment";
    return NULL;
}

static inline int gfg_offscreen_dispatch_complete(
        const gfg_offscreen_dispatch *d) {
    return d && d->create_image && d->destroy_image &&
        d->get_image_requirements && d->get_memory_properties &&
        d->allocate_memory && d->free_memory && d->bind_image_memory;
}

static inline void gfg_offscreen_pool_destroy(
        gfg_offscreen_source_pool *p, VkDevice device,
        const gfg_offscreen_dispatch *d) {
    if (!p || !d || !d->destroy_image || !d->free_memory) return;
    for (uint32_t i = 0; i < GFG_OFFSCREEN_MAX_IMAGES; ++i) {
        if (p->images[i]) d->destroy_image(device, p->images[i], NULL);
        if (p->memory[i]) d->free_memory(device, p->memory[i], NULL);
    }
    memset(p, 0, sizeof(*p));
}

/* Query memory on the exact instance/physical-device dispatch chain.
 * A device-local type is preferred, but any type satisfying memoryTypeBits
 * is correct (the driver determines actual device compatibility). */
static inline uint32_t gfg_offscreen_memory_type(
        const VkPhysicalDeviceMemoryProperties *mp, uint32_t type_bits) {
    uint32_t fallback = UINT32_MAX;
    if (!mp) return UINT32_MAX;
    uint32_t count = mp->memoryTypeCount;
    if (count > VK_MAX_MEMORY_TYPES) count = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < count; ++i) {
        if (!(type_bits & (1u << i))) continue;
        if (mp->memoryTypes[i].propertyFlags &
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

/* Transactional GPU allocation: one image and one dedicated backing memory
 * allocation per source buffer, with exact rollback on any failure.
 * No app image is externally visible until all images are bound.
 *
 * This does not satisfy a complete virtual swapchain by itself. The real
 * layer must hook image enumeration/acquire/present, image layouts, queue
 * waits, image/view lifetimes and recreation before enabling it for games.
 */
static inline VkResult gfg_offscreen_pool_create(
        gfg_offscreen_source_pool *out, VkDevice device,
        VkPhysicalDevice physical, const VkSwapchainCreateInfoKHR *ci,
        const gfg_offscreen_dispatch *d) {
    if (!out || !device || !physical || !gfg_offscreen_dispatch_complete(d))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (out->count) return VK_ERROR_INITIALIZATION_FAILED;
    if (gfg_offscreen_unsupported(ci))
        return VK_ERROR_FEATURE_NOT_PRESENT;

    gfg_offscreen_source_pool temp = {0};
    VkPhysicalDeviceMemoryProperties properties = {0};
    d->get_memory_properties(physical, &properties);

    VkImageCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = ci->imageFormat;
    info.extent = (VkExtent3D){ci->imageExtent.width, ci->imageExtent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < ci->minImageCount; ++i) {
        result = d->create_image(device, &info, NULL, &temp.images[i]);
        if (result != VK_SUCCESS) goto rollback;
        VkMemoryRequirements requirements = {0};
        d->get_image_requirements(device, temp.images[i], &requirements);
        uint32_t memory_type = gfg_offscreen_memory_type(
                &properties, requirements.memoryTypeBits);
        if (memory_type == UINT32_MAX || !requirements.size) {
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            goto rollback;
        }
        VkMemoryAllocateInfo alloc = {0};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = memory_type;
        result = d->allocate_memory(device, &alloc, NULL, &temp.memory[i]);
        if (result != VK_SUCCESS) goto rollback;
        result = d->bind_image_memory(device, temp.images[i], temp.memory[i], 0);
        if (result != VK_SUCCESS) goto rollback;
    }
    temp.count = ci->minImageCount;
    temp.extent = ci->imageExtent;
    temp.format = ci->imageFormat;
    temp.usage = info.usage;
    *out = temp;
    return VK_SUCCESS;
rollback:
    gfg_offscreen_pool_destroy(&temp, device, d);
    return result;
}
#endif

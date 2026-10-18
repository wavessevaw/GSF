/* SPDX-License-Identifier: GPL-3.0-or-later
 * Prefer CPU-cached Vulkan memory for GPU->CPU image readback.
 * HOST_COHERENT alone often implies WC/uncached, disastrous for CPU reads.
 */
#ifndef GFG_NATIVE_MEMORY_POLICY_H
#define GFG_NATIVE_MEMORY_POLICY_H
#include <stdint.h>
#include <vulkan/vulkan.h>
static inline int gfg_native_readback_memory_type(
    const VkPhysicalDeviceMemoryProperties *props, uint32_t compatible,
    uint32_t *index, int *coherent, int *cached) {
    /* Readback: cached + noncoherent, cached + coherent, then uncached
     * coherent/other as fallback. vkInvalidateMappedMemoryRanges is called
     * before reading a noncoherent mapping. */
    for(int pass=0;pass<4;pass++) {
        for(uint32_t i=0;i<props->memoryTypeCount && i<32;i++) {
            const VkMemoryPropertyFlags f=props->memoryTypes[i].propertyFlags;
            if(!(compatible & (1u<<i)) ||
               !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                continue;
            const int is_cached=!!(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            const int is_coherent=!!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if(pass==0 && !(is_cached && !is_coherent))continue;
            if(pass==1 && !(is_cached && is_coherent))continue;
            if(pass==2 && !(!is_cached && is_coherent))continue;
            if(pass==3 && !(!is_cached && !is_coherent))continue;
            *index=i;*coherent=is_coherent;*cached=is_cached;
            return 1;
        }
    }
    return 0;
}
#endif

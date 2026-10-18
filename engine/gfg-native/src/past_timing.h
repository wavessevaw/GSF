/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GFG_PAST_TIMING_H
#define GFG_PAST_TIMING_H
#include <vulkan/vulkan.h>

/* Older Gamescope erases the INPUT capacity, rather than the number copied.
 * A speculative capacity of 16 corrupts its vector when fewer records exist.
 * Query with input zero (also safe with that implementation), then consume at
 * most the available count. The caller serializes this consumer and presents
 * with output_wsi; GOOGLE IDs belong exclusively to this bridge.
 */
static inline VkResult gfg_past_timing_read(PFN_vkGetPastPresentationTimingGOOGLE next,
    VkDevice device, VkSwapchainKHR swapchain, uint32_t capacity,
    VkPastPresentationTimingGOOGLE *past, uint32_t *written) {
    uint32_t available=0;
    *written=0;
    VkResult result=next(device,swapchain,&available,NULL);
    if(result!=VK_SUCCESS && result!=VK_INCOMPLETE)return result;
    if(!available || !capacity)return VK_SUCCESS;
    uint32_t count=available<capacity?available:capacity;
    result=next(device,swapchain,&count,past);
    if(result==VK_SUCCESS || result==VK_INCOMPLETE)
        *written=count<capacity?count:capacity;
    return result;
}
#endif

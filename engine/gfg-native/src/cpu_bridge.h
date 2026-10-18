/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GFG_CPU_BRIDGE_H
#define GFG_CPU_BRIDGE_H
#include <vulkan/vulkan.h>

typedef struct gfg_cpu_fg gfg_cpu_fg;
gfg_cpu_fg *gfg_cpu_fg_create(
    VkInstance instance, PFN_vkGetInstanceProcAddr gipa,
    VkPhysicalDevice physical, VkDevice device, PFN_vkGetDeviceProcAddr gdpa,
    VkSwapchainKHR swapchain, VkExtent2D extent, VkFormat format,
    VkQueue queue, uint32_t family);
VkResult gfg_cpu_fg_present(gfg_cpu_fg *fg, const VkPresentInfoKHR *original,
                            PFN_vkQueuePresentKHR next_present);
void gfg_cpu_fg_destroy(gfg_cpu_fg *fg);
uint64_t gfg_cpu_fg_generated(const gfg_cpu_fg *fg);
#endif

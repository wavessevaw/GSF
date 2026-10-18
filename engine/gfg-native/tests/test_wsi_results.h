/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test app WSI statuses. VK_SUBOPTIMAL_KHR is a successful acquire/present
 * with a usable image. VK_ERROR_OUT_OF_DATE_KHR requires swapchain rebuild.
 * This test does NOT yet recreate a swapchain; it must fail clearly.
 */
#ifndef GFG_TEST_WSI_RESULTS_H
#define GFG_TEST_WSI_RESULTS_H
#include <vulkan/vulkan.h>
static inline int gfg_test_wsi_result_usable(VkResult result) {
    return result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR;
}
static inline int gfg_test_wsi_recreate_required(VkResult result) {
    return result==VK_ERROR_OUT_OF_DATE_KHR;
}
#endif

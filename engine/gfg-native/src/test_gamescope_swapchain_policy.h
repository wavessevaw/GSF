/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only SteamOS Gamescope WSI present-mode bridge.
 *
 * Gamescope may rewrite an app FIFO request to MAILBOX before our explicit
 * Vulkan layer sees the create. Virtual source image retirement and the
 * independent output owner require the FIFO schedule in this experiment.
 * Never permit this translation outside the isolated opt-in live test.
 * This does NOT prove the compositor's physical OLED scanout timing.
 */
#ifndef GFG_TEST_GAMESCOPE_SWAPCHAIN_POLICY_H
#define GFG_TEST_GAMESCOPE_SWAPCHAIN_POLICY_H
#include <vulkan/vulkan.h>
static inline int gfg_test_gamescope_normalize_fifo(
        const VkSwapchainCreateInfoKHR *original,
        int live_test,int gaming_mode,
        VkSwapchainCreateInfoKHR *result) {
    if(!original || !result)return 0;
    *result=*original;
    if(!live_test || !gaming_mode ||
       original->presentMode!=VK_PRESENT_MODE_MAILBOX_KHR)
        return 0;
    result->presentMode=VK_PRESENT_MODE_FIFO_KHR;
    return 1;
}
/* The real Gamescope layer rewrites downstream driver WSI to MAILBOX
 * and injects VkSwapchainPresentModesCreateInfoEXT containing MAILBOX.
 * An experimental virtual app source may use FIFO but the downstream WSI
 * MUST retain Gamescope's MAILBOX protocol and its pNext mode list.
 *
 * Only the exact known one-node maintenance1 chain is accepted. A bad or
 * unknown chain is rejected, never ignored/stripped on the real WSI.
 *
 * result: 1 = adapted, 0 = no adapter needed, -1 = invalid chain.
 */
static inline int gfg_test_gamescope_prepare_pair(
        const VkSwapchainCreateInfoKHR *input,
        int live_test,int gaming_mode,
        VkSwapchainCreateInfoKHR *virtual_source,
        VkSwapchainCreateInfoKHR *real_output,
        const char **reason) {
    if(reason)*reason=NULL;
    if(!input || !virtual_source || !real_output)return -1;
    *virtual_source=*input;
    *real_output=*input;
    if(!live_test || !gaming_mode ||
       input->presentMode!=VK_PRESENT_MODE_MAILBOX_KHR)return 0;

    if(input->pNext) {
        const VkBaseInStructure *node=
            (const VkBaseInStructure *)input->pNext;
        if(node->sType!=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT) {
            if(reason)*reason="unsupported-maintenance1-pnext-type";
            return -1;
        }
        if(node->pNext) {
            if(reason)*reason="unsupported-maintenance1-chain-tail";
            return -1;
        }
        const VkSwapchainPresentModesCreateInfoEXT *modes=
            (const VkSwapchainPresentModesCreateInfoEXT *)node;
        if(modes->presentModeCount!=1 || !modes->pPresentModes ||
           modes->pPresentModes[0]!=VK_PRESENT_MODE_MAILBOX_KHR) {
            if(reason)*reason="unsupported-maintenance1-present-modes";
            return -1;
        }
        /* Real-output info preserves the original pNext without editing
         * the caller's storage or the supported MAILBOX mode array. */
    }
    virtual_source->pNext=NULL; /* only private, non-WSI GPU images */
    virtual_source->presentMode=VK_PRESENT_MODE_FIFO_KHR;
    return 1;
}
#endif

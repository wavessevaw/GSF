/* SPDX-License-Identifier: GPL-3.0-or-later
 * Regression for direct REAL WSI frame-insertion qualification.
 * The app's VkSwapchainKHR remains REAL; no virtual acquire contract.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include "../src/swapchain_chain.h"

int main(void) {
    /* 2x reserves the deferred original plus one intermediate,
     * 3x the deferred original plus two intermediates. */
    assert(gfg_real_wsi_extra_images("2")==2u);
    assert(gfg_real_wsi_extra_images("3")==3u);
    assert(gfg_real_wsi_extra_images(NULL)==3u);
    assert(gfg_real_wsi_can_extend(2,0,2));
    assert(gfg_real_wsi_can_extend(2,5,3));
    assert(!gfg_real_wsi_can_extend(2,4,3));
    assert(!gfg_real_wsi_can_extend(UINT32_MAX,0,3));
    assert(!gfg_real_wsi_can_extend(2,0,0));

    VkSwapchainCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .presentMode=VK_PRESENT_MODE_MAILBOX_KHR};
    gfg_create_chain copy={0};
    assert(gfg_create_fifo(&copy,&ci));
    assert(ci.presentMode==VK_PRESENT_MODE_FIFO_KHR);
    assert(ci.pNext==NULL);

    /* Gamescope's downstream driver-only MAILBOX mode list cannot
     * be rewritten to FIFO, even under explicit user opt-in. */
    VkPresentModeKHR mailbox=VK_PRESENT_MODE_MAILBOX_KHR;
    VkSwapchainPresentModesCreateInfoEXT modes={
        .sType=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
        .presentModeCount=1,.pPresentModes=&mailbox};
    ci.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
    ci.pNext=&modes;
    assert(!gfg_create_fifo(&copy,&ci));
    assert(ci.presentMode==VK_PRESENT_MODE_MAILBOX_KHR);
    assert(ci.pNext==&modes);
    assert(modes.pPresentModes==&mailbox);

    /* An app-facing list with FIFO offered is safely cloned;
     * never overwrite caller-owned pNext arrays. */
    VkPresentModeKHR options[2]={VK_PRESENT_MODE_MAILBOX_KHR,VK_PRESENT_MODE_FIFO_KHR};
    modes.presentModeCount=2;
    modes.pPresentModes=options;
    assert(gfg_create_fifo(&copy,&ci));
    assert(ci.presentMode==VK_PRESENT_MODE_FIFO_KHR);
    const VkSwapchainPresentModesCreateInfoEXT *normalized=ci.pNext;
    assert(normalized!=&modes);
    assert(normalized->presentModeCount==1);
    assert(normalized->pPresentModes[0]==VK_PRESENT_MODE_FIFO_KHR);
    assert(modes.presentModeCount==2 && modes.pPresentModes==options);

    /* Unknown node before mode list cannot be safely relinked. */
    VkBaseInStructure unknown={
        .sType=(VkStructureType)0x7ffffffe,.pNext=(const VkBaseInStructure *)&modes};
    ci.pNext=&unknown;ci.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
    assert(!gfg_create_fifo(&copy,&ci));
    assert(ci.pNext==&unknown);
    assert(ci.presentMode==VK_PRESENT_MODE_MAILBOX_KHR);

    puts("GSF_REAL_WSI_DIRECT_POLICY fifo_app_layer=1 mailbox_downstream_preserved=1 extra_2x=2 extra_3x=3 PASS");
    return 0;
}

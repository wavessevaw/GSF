/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hardware-independent check for the Steam Deck's actual swapchain request.
 * NEVER send present-id2 flags to an ICD lacking the corresponding extension.
 * The test exercises our eligibility filter only, not a fabricated surface.
 */
#define _GNU_SOURCE /* layer.c uses clock_gettime; must precede the first libc header */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/layer.c"

int main(void) {
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .flags = VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR |
                 VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageExtent = {1280, 800},
        .imageArrayLayers = 1,
        .imageUsage = 0x1b,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
    };
    assert(ci.flags == 0xc0);
    assert((int)ci.imageFormat == 44);
    assert(native_ineligible_reason(&ci) == NULL);
    assert(native_copy_usage_ready(ci.imageUsage));
    assert(gfg_real_wsi_can_extend(3, 0, 2));  /* 2x */
    assert(gfg_real_wsi_can_extend(3, 5, 2));
    assert(!gfg_real_wsi_can_extend(3, 4, 2));
    assert(gfg_real_wsi_can_extend(3, 6, 3));  /* 3x */
    assert(!gfg_real_wsi_can_extend(3, 5, 3));
    assert(!gfg_real_wsi_can_extend(5, 4, 3));
    puts("GFG native: actual Steam Deck 0xc0/44/0x1b swapchain eligible PASS");
    ci.flags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR;
    assert(!strcmp(native_ineligible_reason(&ci), "unsupported-swapchain-flags"));
    ci.flags = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    assert(!strcmp(native_ineligible_reason(&ci), "mutable-format-list-missing"));
    VkFormat aliases[2]={VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_B8G8R8A8_SRGB};
    const VkImageFormatListCreateInfo list={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount=2,.pViewFormats=aliases};
    ci.pNext=&list;
    assert(native_ineligible_reason(&ci)==NULL);
    ci.flags|=VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR|VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    assert(native_ineligible_reason(&ci)==NULL);
    aliases[1]=VK_FORMAT_R8G8B8A8_SRGB;
    assert(!strcmp(native_ineligible_reason(&ci),"mutable-format-alias-unsupported"));
    aliases[0]=aliases[1]=VK_FORMAT_B8G8R8A8_SRGB;
    assert(!strcmp(native_ineligible_reason(&ci),"mutable-format-base-missing"));
    aliases[0]=VK_FORMAT_B8G8R8A8_UNORM;aliases[1]=VK_FORMAT_B8G8R8A8_SRGB;
    ci.imageFormat=VK_FORMAT_R8G8B8A8_UNORM;
    aliases[0]=VK_FORMAT_R8G8B8A8_UNORM;aliases[1]=VK_FORMAT_R8G8B8A8_SRGB;
    assert(native_ineligible_reason(&ci)==NULL);
    aliases[0]=VK_FORMAT_B8G8R8A8_UNORM;aliases[1]=VK_FORMAT_B8G8R8A8_SRGB;
    ci.imageFormat=VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    assert(!strcmp(native_ineligible_reason(&ci),"mutable-format-family-unsupported"));
    ci.imageFormat=VK_FORMAT_B8G8R8A8_UNORM;ci.pNext=NULL;
    ci.flags = 0x1000;
    assert(!strcmp(native_ineligible_reason(&ci), "unsupported-swapchain-flags"));
    ci.flags = VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    assert(native_ineligible_reason(&ci) == NULL);
    ci.flags = VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
    assert(native_ineligible_reason(&ci) == NULL);
    ci.flags = 0;
    assert(native_ineligible_reason(&ci) == NULL);
    ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    assert(!strcmp(native_ineligible_reason(&ci), "non-fifo-present-mode"));
    ci.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    assert(!strcmp(native_ineligible_reason(&ci), "non-fifo-present-mode"));
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    assert(!native_copy_usage_ready(ci.imageUsage));
    ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    assert(!strcmp(native_ineligible_reason(&ci), "sharing-mode"));
    puts("GFG native: present-extension mask and protection refusal PASS");
    ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.imageFormat=VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    setenv("GFG_NATIVE_GPU_BRIDGE","1",1);
    assert(native_ineligible_reason(&ci)==NULL);
    ci.imageColorSpace=VK_COLOR_SPACE_HDR10_ST2084_EXT;
    assert(native_ineligible_reason(&ci)==NULL);
    ci.imageColorSpace=VK_COLOR_SPACE_HDR10_HLG_EXT;
    assert(!strcmp(native_ineligible_reason(&ci),"unsupported-color-space"));
    ci.imageColorSpace=VK_COLOR_SPACE_HDR10_ST2084_EXT;ci.imageFormat=VK_FORMAT_B8G8R8A8_UNORM;
    assert(!strcmp(native_ineligible_reason(&ci),"unsupported-pq-format"));
    ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;ci.imageFormat=VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    /* Exactly the 2026-10-17 Gamescope/OLED crash scenario: packed 10-bit
     * WSI must NOT proceed into GSF's invalid physical-device query. */
    setenv("VK_INSTANCE_LAYERS","VK_LAYER_GFG_native:VK_LAYER_FROG_gamescope_wsi_x86_64",1);
    assert(native_gamescope_packed10_unsafe(&ci));
    assert(!strcmp(native_ineligible_reason(&ci), "gamescope-packed10-physical-dispatch-unverified"));
    ci.imageFormat=VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    assert(native_gamescope_packed10_unsafe(&ci));
    assert(!strcmp(native_ineligible_reason(&ci), "gamescope-packed10-physical-dispatch-unverified"));
    ci.imageFormat=VK_FORMAT_B8G8R8A8_UNORM;
    assert(!native_gamescope_packed10_unsafe(&ci));
    assert(native_ineligible_reason(&ci)==NULL);
    unsetenv("VK_INSTANCE_LAYERS");
    ci.imageFormat=VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    assert(native_ineligible_reason(&ci)==NULL);

    VkPresentModeKHR mailbox=VK_PRESENT_MODE_MAILBOX_KHR;
    const VkPresentModeKHR allowed_modes[2]={VK_PRESENT_MODE_MAILBOX_KHR,VK_PRESENT_MODE_FIFO_KHR};
    const VkSwapchainPresentModesCreateInfoEXT modes={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
        .presentModeCount=2,.pPresentModes=allowed_modes};
    const VkImageFormatListCreateInfo formats={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,.pNext=&modes};
    ci.pNext=&formats;ci.presentMode=mailbox;
    VkSwapchainCreateInfoKHR effective=ci;gfg_create_chain copy;
    assert(gfg_create_fifo(&copy,&effective));
    assert(ci.pNext==&formats && formats.pNext==&modes &&
           modes.presentModeCount==2 && modes.pPresentModes[0]==mailbox);
    const VkImageFormatListCreateInfo *f=effective.pNext;
    const VkSwapchainPresentModesCreateInfoEXT *m=f->pNext;
    assert(effective.presentMode==VK_PRESENT_MODE_FIFO_KHR && f!=&formats && m!=&modes);
    assert(m->presentModeCount==1 && m->pPresentModes[0]==VK_PRESENT_MODE_FIFO_KHR);
    /* MAILBOX-only metadata from downstream Gamescope is not forceable to FIFO. */
    VkSwapchainPresentModesCreateInfoEXT downstream=modes;
    downstream.presentModeCount=1;
    downstream.pPresentModes=&mailbox;
    effective=ci;effective.pNext=&downstream;
    assert(!gfg_create_fifo(&copy,&effective));
    assert(effective.presentMode==mailbox && effective.pNext==&downstream);
    VkBaseInStructure unknown={.sType=(VkStructureType)1999999999,.pNext=(const void*)&modes};
    effective=ci;effective.pNext=&unknown;
    assert(!gfg_create_fifo(&copy,&effective));
    assert(effective.pNext==&unknown && effective.presentMode==mailbox);
    unsetenv("GFG_NATIVE_GPU_BRIDGE");
    puts("GFG native: HDR10 eligibility and immutable FIFO create-mode prefix PASS");
    return 0;
}

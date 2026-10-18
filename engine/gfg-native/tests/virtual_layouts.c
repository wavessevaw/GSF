/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/virtual_layouts.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static VkImage img(uintptr_t n) {return (VkImage)n;}
static void barrier_isolation(void) {
    VkImage virtuals[]={img(0x100),img(0x101)};
    gfg_virtual_layout_map m={.images=virtuals,.count=2};
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image=virtuals[0],
        .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkImageMemoryBarrier out={0};
    assert(gfg_vlayout_legacy(&m,&b,&out));
    assert(b.newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(out.newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(out.oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    assert(m.translated_new==1 && m.translated_old==0);
    b.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    assert(gfg_vlayout_legacy(&m,&b,&out));
    assert(out.oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(out.newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    assert(m.translated_new==1 && m.translated_old==1);
    /* Another swapchain's REAL present layout must NEVER be rewritten. */
    b.image=img(0x300);
    b.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    assert(gfg_vlayout_legacy(&m,&b,&out));
    assert(out.oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(out.newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(m.translated_new==1 && m.translated_old==1);
    VkImageMemoryBarrier2 sync2={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .image=virtuals[1],.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkImageMemoryBarrier2 sync2out={0};
    assert(gfg_vlayout_sync2(&m,&sync2,&sync2out));
    assert(sync2out.oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(sync2out.newLayout==VK_IMAGE_LAYOUT_GENERAL);
    assert(sync2.oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(m.translated_old==2);
    puts("virtual PRESENT layout: legacy+sync2 virtual only, downstream untouched PASS");
}
static void renderpass_gating(void) {
    VkAttachmentDescription a={.format=VK_FORMAT_B8G8R8A8_UNORM,
        .initialLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentDescription b={0};
    assert(gfg_vlayout_renderpass_attachment(&a,&b,0));
    assert(b.initialLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(b.finalLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(gfg_vlayout_renderpass_attachment(&a,&b,1));
    assert(b.initialLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(b.finalLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(a.finalLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VkAttachmentDescription2 c={.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentDescription2 d={0};
    assert(gfg_vlayout_renderpass2_attachment(&c,&d,0));
    assert(d.finalLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(gfg_vlayout_renderpass2_attachment(&c,&d,1));
    assert(d.finalLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    puts("virtual PRESENT layout: renderpass and renderpass2 explicit isolation gate PASS");
}
int main(void) {
    barrier_isolation();renderpass_gating();
    return 0;
}

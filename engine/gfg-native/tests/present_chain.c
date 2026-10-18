/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../src/present_chain.h"

typedef struct {
    VkPresentInfoKHR app;
    VkSwapchainPresentFenceInfoEXT fence;
    VkSwapchainPresentModeInfoEXT mode;
    VkPresentRegionsKHR regions;
    VkPresentRegionKHR region;
    VkPresentId2KHR id;
    VkFence handle;
    VkPresentModeKHR requested;
    uint64_t app_id;
} app_data;

int main(void) {
    app_data *input = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(input != MAP_FAILED);
    input->app_id = 77;
    input->handle = (VkFence)(uintptr_t)123;
    input->requested = VK_PRESENT_MODE_MAILBOX_KHR;
    input->id = (VkPresentId2KHR) {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .swapchainCount = 1, .pPresentIds = &input->app_id};
    input->regions = (VkPresentRegionsKHR) {.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,
        .pNext = &input->id, .swapchainCount = 1, .pRegions = &input->region};
    input->mode = (VkSwapchainPresentModeInfoEXT) {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .pNext = &input->regions, .swapchainCount = 1, .pPresentModes = &input->requested};
    input->fence = (VkSwapchainPresentFenceInfoEXT) {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .pNext = &input->mode, .swapchainCount = 1, .pFences = &input->handle};
    input->app = (VkPresentInfoKHR) {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = &input->fence, .swapchainCount = 1};
    unsigned char original[sizeof(*input)];
    memcpy(original, input, sizeof(original));
    assert(mprotect(input, 4096, PROT_READ) == 0);

    gfg_present_chain out;
    assert(!gfg_pc_prepare(&out, &input->app, 2));
    assert(out.app_id == 77 && out.count == 3 && out.id_index == 2);
    assert(out.dynamic_mode && out.requested_mode == VK_PRESENT_MODE_MAILBOX_KHR);
    gfg_pc_set_id(&out, 2, 230);
    const VkSwapchainPresentFenceInfoEXT *f = gfg_pc_head(&out);
    assert(f->pFences == &input->handle && *f->pFences == input->handle);
    const VkSwapchainPresentModeInfoEXT *m = f->pNext;
    assert(m->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT);
    assert(*m->pPresentModes == VK_PRESENT_MODE_FIFO_KHR);
    const VkPresentId2KHR *id = m->pNext;
    assert(id->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR && !id->pNext);
    assert(*id->pPresentIds == 230 && input->app_id == 77);
    assert(memcmp(input, original, sizeof(original)) == 0);
    assert(!gfg_pc_prepare(&out, &input->app, 0));
    assert(out.nodes[out.id_index].id2.pPresentIds == &input->app_id);
    assert(*out.nodes[out.id_index].id2.pPresentIds == 77); /* app-owned IDs survive unchanged */

    VkPresentInfoKHR app = input->app;
    VkSwapchainPresentFenceInfoEXT fence = input->fence;
    VkSwapchainPresentModeInfoEXT mode = input->mode;
    mode.pNext = NULL;
    fence.pNext = &mode;
    app.pNext = &fence;
    assert(!gfg_pc_prepare(&out, &app, 0));
    assert(out.count == 2 && !out.app_id);
    gfg_pc_set_id(&out, 1, 231);
    assert(out.count == 3);
    assert(*out.nodes[2].id1.pPresentIds == 231 && !out.nodes[2].id1.pNext);

    uint64_t old_id = 900;
    VkPresentIdKHR v1 = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .pNext = &fence, .swapchainCount = 1, .pPresentIds = &old_id};
    app.pNext = &v1;
    assert(!gfg_pc_prepare(&out, &app, 1));
    gfg_pc_set_id(&out, 1, 1000);
    assert(*out.nodes[0].id1.pPresentIds == 1000 && old_id == 900);
    assert(out.nodes[1].fence.pFences == fence.pFences);

    VkPresentTimeGOOGLE time = {.presentID = 7, .desiredPresentTime = 12345678};
    VkPresentTimesInfoGOOGLE times = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
        .pNext = &fence, .swapchainCount = 1, .pTimes = &time};
    app.pNext = &times;
    assert(!gfg_pc_prepare(&out, &app, 0));
    assert(out.nodes[0].times.pTimes == &time && time.desiredPresentTime == 12345678);

    VkPresentTimingInfoEXT timing = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
        .targetTime = 12345678};
    VkPresentTimingsInfoEXT timings = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
        .pNext = &input->id, .swapchainCount = 1, .pTimingInfos = &timing};
    app.pNext = &timings;
    assert(!gfg_pc_prepare(&out, &app, 0));
    assert(out.nodes[0].timings.pTimingInfos == &timing && out.app_id == 77);
    timing.flags = VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT;
    assert(!strcmp(gfg_pc_prepare(&out, &app, 0), "relative-present-timing"));
    VkPresentRegionsKHR full = {.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR, .swapchainCount = 1};
    app.pNext = &full;
    assert(!gfg_pc_prepare(&out, &app, 0) && !gfg_pc_head(&out));

    VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pNext = (void *)&fence};
    app.pNext = &unknown;
    assert(!strcmp(gfg_pc_prepare(&out, &app, 2), "unsupported-present-metadata"));
    assert(out.rejected_type == unknown.sType);
    app.pNext = &v1;
    fence.pNext = &v1; /* cycle/duplicate must fail without walking forever. */
    assert(!strcmp(gfg_pc_prepare(&out, &app, 1), "duplicate-present-metadata"));
    VkPresentInfoKHR passthrough=app;
    unknown.pNext=(const void *)&mode;mode.pNext=&timings;
    app.pNext=&unknown;
    assert(!gfg_pc_fifo_fallback(&out,&app,&passthrough));
    const uint32_t mask=1;
    VkDeviceGroupPresentInfoKHR group={.sType=VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,
        .pNext=&mode,.swapchainCount=1,.pDeviceMasks=&mask,.mode=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR};
    app.pNext=&group;passthrough=app;
    assert(gfg_pc_fifo_fallback(&out,&app,&passthrough));
    const VkDeviceGroupPresentInfoKHR *pg=passthrough.pNext;
    const VkSwapchainPresentModeInfoEXT *pm=pg->pNext;
    assert(pg!=&group && pg->pDeviceMasks==&mask && pm!=&mode);
    assert(pm->pPresentModes[0]==VK_PRESENT_MODE_FIFO_KHR && pm->pNext==&timings);
    assert(mode.pPresentModes[0]==VK_PRESENT_MODE_MAILBOX_KHR && group.pNext==&mode);
    app.pNext = NULL;
    assert(!gfg_pc_prepare(&out, &app, 0) && !gfg_pc_head(&out));
    app.swapchainCount = 2;
    assert(!strcmp(gfg_pc_prepare(&out, &app, 0), "multiple-swapchains"));
    assert(munmap(input, 4096) == 0);
    puts("present-chain: read-only Proton fence/mode/regions/ID chains, ID tail remap, real-only fences/times, FIFO override and unknown fallback PASS");
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual production packet builder with a recording WSI, no display claim.
 */
#define _GNU_SOURCE
#define GFG_NATIVE_WITH_TE
#include "../src/gpu_bridge.c"
#include <assert.h>
static unsigned calls, mode;
static const void *original_chain;
static uint32_t previous_id;
static uint64_t wanted;
static VKAPI_ATTR VkResult VKAPI_CALL recording_wsi(VkQueue queue,const VkPresentInfoKHR *pi) {
    (void)queue;
    if(mode==2) {
        const VkPresentTimesInfoGOOGLE *times=pi->pNext;
        assert(times && times->sType==VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE);
        assert(times->pNext==original_chain && times->swapchainCount==1);
        assert(times->pTimes[0].desiredPresentTime==wanted);
        assert(times->pTimes[0].presentID==++previous_id);
    } else assert(pi->pNext==original_chain);
    ++calls;return VK_SUCCESS;
}
int main(void) {
    VkSwapchainKHR swapchain=(VkSwapchainKHR)(uintptr_t)1;uint32_t image=0;
    const uint64_t id=77;
    const VkPresentIdKHR app_id={.sType=VK_STRUCTURE_TYPE_PRESENT_ID_KHR,.swapchainCount=1,.pPresentIds=&id};
    const VkPresentInfoKHR app={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.pNext=&app_id,
        .swapchainCount=1,.pSwapchains=&swapchain,.pImageIndices=&image};
    original_chain=&app_id;
    gfg_gpu_fg fg={.display_timing=2,.pacing_active=1,.clock={.period_ns=4000000}};
    mode=2;
    for(unsigned k=0;k<3;++k) {
        wanted=(uint64_t)(now_us()*1000.0)+2000000;
        fg.clock.target_ns[k]=wanted;
        assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,k)==VK_SUCCESS);
        assert(app.pNext==&app_id && app_id.pPresentIds[0]==77);
        assert(fg.accepted_ns[k] && fg.google_ids[k]==k+1);
    }
    /* App-owned namespace: preserve its entire chain; no injected GOOGLE IDs. */
    mode=1;fg.display_timing=1;fg.pacing_active=0;
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    mode=0;fg.display_timing=0;
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    assert(calls==5 && previous_id==3);
    puts("presentation-timing: distinct desired G1/G2/R times, immutable chains, namespace ownership and disabled-extension fallback PASS (no scanout)");
}

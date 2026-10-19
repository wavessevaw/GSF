/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual production packet builder with a recording WSI, no display claim.
 */
#define _GNU_SOURCE
#define GFG_NATIVE_WITH_TE
#include "../src/gpu_bridge.c"
#include <assert.h>
#include <sys/stat.h>
static void *queue_pressure(void *arg) {
    const struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    gfg_gpu_fg_set_output_pressure(arg,1);return NULL;
}
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
    gfg_gpu_fg fg={.display_timing=2,.pacing_active=1,.pace_real=1,.clock={.period_ns=4000000}};
    mode=2;
    for(unsigned k=0;k<3;++k) {
        wanted=(uint64_t)(now_us()*1000.0)+2000000;
        fg.clock.target_ns[k]=wanted;
        assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,k)==VK_SUCCESS);
        assert(app.pNext==&app_id && app_id.pPresentIds[0]==77);
        assert(fg.accepted_ns[k] && fg.google_ids[k]==k+1);
    }
    /* A target two seconds away cannot hold a newly arrived real frame. */
    fg.output_wsi=(pthread_mutex_t *)NULL;
    pthread_mutex_t wsi=PTHREAD_MUTEX_INITIALIZER;fg.output_wsi=&wsi;
    fg.clock.target_ns[2]=(uint64_t)(now_us()*1000.0)+2000000000ull;
    wanted=0;
    pthread_t producer;assert(!pthread_create(&producer,NULL,queue_pressure,&fg));
    double start=now_us();
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    assert(now_us()-start<250000.0);
    pthread_join(producer,NULL);pthread_mutex_destroy(&wsi);fg.output_wsi=NULL;
    gfg_gpu_fg_set_output_pressure(&fg,0);
    /* G1 already accepted: C cannot pull paired B into a different slot.
     * Unlike the unbounded fake target above, this is a valid pair plan. */
    fg.clock.source_ns=(uint64_t)(now_us()*1000.0);
    fg.clock.interval_ns=16000000ull;
    fg.clock.target_ns[2]=wanted=fg.clock.source_ns+12000000ull;
    assert(!pthread_create(&producer,NULL,queue_pressure,&fg));
    start=now_us();
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    assert(fg.accepted_ns[2]>=wanted-fg.clock.period_ns/4);
    assert(now_us()-start<250000.0);
    pthread_join(producer,NULL);gfg_gpu_fg_set_output_pressure(&fg,0);
    /* Acquired synthetic image already has a submitted GPU transport. It
     * must retire through WSI without being moved to an earlier phase. */
    fg.clock.source_ns=(uint64_t)(now_us()*1000.0);
    fg.clock.target_ns[0]=wanted=fg.clock.source_ns+8000000ull;
    fg.clock.target_ns[2]=fg.clock.source_ns+12000000ull;
    fg.transport_submit_mask=1;
    assert(!pthread_create(&producer,NULL,queue_pressure,&fg));
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,0)==VK_SUCCESS);
    assert(fg.accepted_ns[0]>=wanted-fg.clock.period_ns/4);
    pthread_join(producer,NULL);gfg_gpu_fg_set_output_pressure(&fg,0);
    fg.transport_submit_mask=0;
    wanted=0;
    fg.diagnostic=1;
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    fg.diagnostic=0;
    /* A missed/absent synthetic must never delay the native frame to a
     * future clock target, including through GOOGLE desiredPresentTime. */
    fg.pace_real=0;start=now_us();
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    /* B1 accepted G1 -> B2 is budget-blocked early. No leaked desiredTime. */
    fg.pace_real=1;
    wanted=(uint64_t)(now_us()*1000.0)+2000000ull;
    fg.clock.target_ns[2]=wanted;
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    gsf_begin_native_pair(&fg); /* same entry reset used before every bypass */
    wanted=0;
    fg.clock.target_ns[2]=(uint64_t)(now_us()*1000.0)+2000000000ull;
    start=now_us();
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    assert(!fg.pace_real && now_us()-start<250000.0);
    assert(now_us()-start<250000.0);
    /* Bounded flushing: multiple small events stay buffered until 250ms. */
    fg.telemetry=tmpfile();assert(fg.telemetry);fg.telemetry_flush_ns=10;
    fputs("first\nsecond\n",fg.telemetry);
    struct stat st;assert(!fstat(fileno(fg.telemetry),&st) && !st.st_size);
    telemetry_flush(&fg,249000010);assert(!fstat(fileno(fg.telemetry),&st) && !st.st_size);
    telemetry_flush(&fg,250000010);assert(!fstat(fileno(fg.telemetry),&st) && st.st_size==13);
    fclose(fg.telemetry);fg.telemetry=NULL;
    /* App-owned namespace: preserve its entire chain; no injected GOOGLE IDs. */
    mode=1;fg.display_timing=1;fg.pacing_active=0;
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    mode=0;fg.display_timing=0;
    assert(output_present(&fg,VK_NULL_HANDLE,&app,recording_wsi,2)==VK_SUCCESS);
    assert(calls==12 && previous_id==10);
    puts("presentation-timing: distinct desired G1/G2/R times, immutable chains, namespace ownership and disabled-extension fallback PASS (no scanout)");
}

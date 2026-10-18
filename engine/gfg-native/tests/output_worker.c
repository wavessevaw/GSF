/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual worker with a controlled backend: caller lifetime, one-job capacity,
 * timeout accounting, real-ID completion and delayed error propagation.
 */
#include "../src/output_worker.c"
#include <assert.h>
#include <sys/mman.h>
#include <stdatomic.h>

static pthread_mutex_t backend_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t backend_changed = PTHREAD_COND_INITIALIZER;
static int entered, release_backend, history_restarts;
static VkResult backend_result;
static uint32_t observed_image;
static unsigned gate_creates,gate_destroys,app_wait_submits;
static atomic_int pressure;
static uint64_t seen_ids[8];
static int require_pressure;
static VKAPI_ATTR VkResult VKAPI_CALL create_gate(VkDevice d,const VkSemaphoreCreateInfo *ci,
        const VkAllocationCallbacks *a,VkSemaphore *s) {
    (void)d;(void)ci;(void)a;*s=(VkSemaphore)(uintptr_t)(1000+gate_creates++);return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_gate(VkDevice d,VkSemaphore s,const VkAllocationCallbacks *a) {
    (void)d;(void)s;(void)a;++gate_destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL consume_app_waits(VkQueue q,uint32_t n,const VkSubmitInfo *si,VkFence f) {
    (void)q;(void)f;assert(n==1 && si->waitSemaphoreCount==1);
    assert(si->pWaitSemaphores[0]==(VkSemaphore)(uintptr_t)1);
    assert(si->signalSemaphoreCount==1 && (uintptr_t)si->pSignalSemaphores[0]>=1000);
    if(require_pressure)assert(atomic_load(&pressure)); /* before host queue submission */
    ++app_wait_submits;return VK_SUCCESS;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL dispatch(VkDevice device,const char *name) {
    (void)device;
    if(!strcmp(name,"vkCreateSemaphore"))return (PFN_vkVoidFunction)create_gate;
    if(!strcmp(name,"vkDestroySemaphore"))return (PFN_vkVoidFunction)destroy_gate;
    if(!strcmp(name,"vkQueueSubmit"))return (PFN_vkVoidFunction)consume_app_waits;
    return NULL;
}
void gfg_gpu_fg_set_output_thread(gfg_gpu_fg *fg, pthread_mutex_t *wsi) { (void)fg; (void)wsi; }
static uint64_t observed_source,observed_admission,observed_serial;
void gfg_gpu_fg_set_source(gfg_gpu_fg *fg,uint64_t capture,uint64_t admission,uint64_t serial) {
    (void)fg;observed_source=capture;observed_admission=admission;observed_serial=serial;
}
void gfg_gpu_fg_set_output_pacing(gfg_gpu_fg *fg,int active) { (void)fg;(void)active; }
void gfg_gpu_fg_set_output_pressure(gfg_gpu_fg *fg,int active) { (void)fg;atomic_store(&pressure,active); }
uint64_t gfg_gpu_fg_source_period_ns(gfg_gpu_fg *fg) {(void)fg;return 33333333ull;}
void gfg_gpu_fg_restart_history(gfg_gpu_fg *fg) { (void)fg; ++history_restarts; }
int gfg_gpu_fg_waiting_image(gfg_gpu_fg *fg) { (void)fg; return 0; }
VkResult gfg_gpu_fg_present(gfg_gpu_fg *fg, VkQueue q, const VkPresentInfoKHR *pi,
        PFN_vkQueuePresentKHR next, const gfg_present_id_hooks *ids) {
    (void)fg; (void)q; (void)next; (void)ids;
    assert(pi->waitSemaphoreCount==1 && (uintptr_t)pi->pWaitSemaphores[0]>=1000);
    const VkPresentTimingsInfoEXT *times=pi->pNext;
    assert(times && times->sType==VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
    assert(times->pTimingInfos==NULL && times->swapchainCount==1);
    const VkPresentId2KHR *id=times->pNext;
    assert(id && id->sType==VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR && !id->pNext);
    pthread_mutex_lock(&backend_lock);
    observed_image = pi->pImageIndices[0];seen_ids[entered]=id->pPresentIds[0];++entered;
    pthread_cond_broadcast(&backend_changed);
    while (!release_backend) pthread_cond_wait(&backend_changed,&backend_lock);
    VkResult r = backend_result;
    pthread_mutex_unlock(&backend_lock); return r;
}
static int third_done,third_handled;
static gfg_output *third_out;
static VkPresentInfoKHR *third_pi;
static void *third_submit(void *unused) {
    (void)unused;
    assert(gfg_output_submit(third_out,third_pi,0,&third_handled)==VK_SUCCESS);
    pthread_mutex_lock(&backend_lock);third_done=1;
    pthread_cond_broadcast(&backend_changed);pthread_mutex_unlock(&backend_lock);return NULL;
}

int main(void) {
    struct payload {
        VkPresentInfoKHR pi;
        VkPresentId2KHR id;
        VkSwapchainPresentFenceInfoEXT fence;
        VkSwapchainPresentModeInfoEXT mode;
        VkPresentTimingsInfoEXT times;
        VkPresentTimingInfoEXT timing;
        VkSemaphore wait;
        VkSwapchainKHR swapchain;
        VkFence present_fence;
        VkPresentModeKHR present_mode;
        uint32_t index;
        uint64_t present_id;
        VkResult result;
    } *p = mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
    assert(p != MAP_FAILED);
    p->index=3; p->present_id=42; p->present_mode=VK_PRESENT_MODE_IMMEDIATE_KHR;
    p->wait=(VkSemaphore)(uintptr_t)1; p->swapchain=(VkSwapchainKHR)(uintptr_t)2;
    p->present_fence=(VkFence)(uintptr_t)3;
    p->timing=(VkPresentTimingInfoEXT){.sType=VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,.targetTime=12345};
    p->times=(VkPresentTimingsInfoEXT){.sType=VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
        .swapchainCount=1,.pTimingInfos=&p->timing};
    p->mode=(VkSwapchainPresentModeInfoEXT){.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .pNext=&p->times,.swapchainCount=1,.pPresentModes=&p->present_mode};
    p->fence=(VkSwapchainPresentFenceInfoEXT){.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .pNext=&p->mode,.swapchainCount=1,.pFences=&p->present_fence};
    p->id=(VkPresentId2KHR){.sType=VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,.pNext=&p->fence,
        .swapchainCount=1,.pPresentIds=&p->present_id};
    p->pi=(VkPresentInfoKHR){.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.pNext=&p->id,
        .waitSemaphoreCount=1,.pWaitSemaphores=&p->wait,.swapchainCount=1,
        .pSwapchains=&p->swapchain,.pImageIndices=&p->index,.pResults=&p->result};
    output_job *j=copy_job(&p->pi,0); assert(j);
    assert(mprotect(p,4096,PROT_NONE)==0);
    assert(j->pi.pResults==NULL && j->pi.pImageIndices[0]==3 && j->pi.pWaitSemaphores[0]==(VkSemaphore)(uintptr_t)1);
    const VkPresentId2KHR *id=j->pi.pNext;
    const VkSwapchainPresentFenceInfoEXT *fence=id->pNext;
    const VkSwapchainPresentModeInfoEXT *mode=fence->pNext;
    const VkPresentTimingsInfoEXT *times=mode->pNext;
    assert(id->pPresentIds[0]==42 && fence->pFences[0]==(VkFence)(uintptr_t)3);
    assert(mode->pPresentModes[0]==VK_PRESENT_MODE_FIFO_KHR && times->pTimingInfos[0].targetTime==12345);
    free(j);
    assert(mprotect(p,4096,PROT_READ|PROT_WRITE)==0);
    p->timing.pNext=&p->id; assert(copy_job(&p->pi,0)==NULL); p->timing.pNext=NULL;

    p->times.pTimingInfos=NULL;
    j=copy_job(&p->pi,0); assert(j);
    assert(mprotect(p,4096,PROT_NONE)==0);
    id=j->pi.pNext; fence=id->pNext; mode=fence->pNext; times=mode->pNext;
    assert(times->pTimingInfos==NULL && times->swapchainCount==1 && !times->pNext);
    assert(id->pPresentIds[0]==42 && j->pi.pResults==NULL);
    free(j);
    assert(mprotect(p,4096,PROT_READ|PROT_WRITE)==0);

    pthread_mutex_t wsi=PTHREAD_MUTEX_INITIALIZER;
    gfg_output *out=gfg_output_create((gfg_gpu_fg *)(uintptr_t)1,VK_NULL_HANDLE,NULL,NULL,&wsi,
                                     VK_NULL_HANDLE,dispatch,5,NULL); assert(out);
    gfg_output_acquired(out,3,(VkSemaphore)(uintptr_t)7);
    p->pi.pNext=&p->times; p->times.pNext=&p->id; p->id.pNext=NULL;
    int handled=0;
    assert(gfg_output_submit(out,&p->pi,0,&handled)==VK_SUCCESS && handled && p->result==VK_SUCCESS);
    assert(app_wait_submits==1); /* already consumed even while backend is paused */
    assert(!gfg_output_sem_reusable(out,(VkSemaphore)(uintptr_t)7));
    pthread_mutex_lock(&backend_lock);
    while(!entered) pthread_cond_wait(&backend_changed,&backend_lock);
    assert(observed_image==3); pthread_mutex_unlock(&backend_lock);
    require_pressure=1;
    p->index=4; p->present_id=43;
    assert(gfg_output_submit(out,&p->pi,0,&handled)==VK_SUCCESS && handled);
    assert(app_wait_submits==2);
    /* A third acquired real image must be admitted while the first backend
     * is deliberately frozen. The former one-pending mailbox blocks here. */
    p->index=2;p->present_id=44;third_out=out;third_pi=&p->pi;
    pthread_t producer;assert(!pthread_create(&producer,NULL,third_submit,NULL));
    struct timespec limit;clock_gettime(CLOCK_REALTIME,&limit);++limit.tv_sec;
    pthread_mutex_lock(&backend_lock);
    while(!third_done)if(pthread_cond_timedwait(&backend_changed,&backend_lock,&limit)==ETIMEDOUT)break;
    assert(third_done && third_handled && !release_backend);
    pthread_mutex_unlock(&backend_lock);pthread_join(producer,NULL);
    assert(app_wait_submits==3 && atomic_load(&pressure));
    /* Caller storage is now invalid. Both the executing and pending job must
     * retain scalar arrays, ID, semaphore handles and no pResults pointer. */
    assert(mprotect(p,4096,PROT_NONE)==0);
    assert(gfg_output_drain(out,42,0,NULL)==VK_TIMEOUT);
    uint64_t remaining=99;
    assert(gfg_output_drain(out,0,1000000,&remaining)==VK_TIMEOUT && remaining==0);
    pthread_mutex_lock(&out->lock); assert(out->busy && out->queued==2 && out->submitted==3 && out->peak_queued<=out->image_count);
    out->jobs[out->head]->queued_ns=tick_ns()-100000000ull; /* deterministic expired job */
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_lock(&backend_lock); release_backend=1;
    pthread_cond_broadcast(&backend_changed); pthread_mutex_unlock(&backend_lock);
    assert(gfg_output_drain(out,0,1000000000,&remaining)==VK_SUCCESS);
    assert(out->completed==3 && out->completed_id==44 && observed_image==2);
    assert(seen_ids[0]==42 && seen_ids[1]==43 && seen_ids[2]==44 && !atomic_load(&pressure));
    assert(out->stale==1 && history_restarts==1);
    assert(observed_serial==3 && observed_source<=observed_admission && observed_admission>0);
    assert(gfg_output_sem_reusable(out,(VkSemaphore)(uintptr_t)7));
    gfg_output_destroy(out);

    assert(mprotect(p,4096,PROT_READ|PROT_WRITE)==0);
    backend_result=VK_ERROR_DEVICE_LOST;
    require_pressure=0;
    out=gfg_output_create((gfg_gpu_fg *)(uintptr_t)1,VK_NULL_HANDLE,NULL,NULL,&wsi,
                          VK_NULL_HANDLE,dispatch,5,NULL); assert(out);
    assert(gfg_output_submit(out,&p->pi,0,&handled)==VK_SUCCESS && handled);
    assert(gfg_output_drain(out,0,1000000000,NULL)==VK_ERROR_DEVICE_LOST);
    assert(gfg_output_submit(out,&p->pi,0,&handled)==VK_ERROR_DEVICE_LOST && handled);
    assert(p->result==VK_ERROR_DEVICE_LOST);
    assert(gfg_output_status(out)==VK_ERROR_DEVICE_LOST && app_wait_submits==4);
    gfg_output_destroy(out); pthread_mutex_destroy(&wsi); munmap(p,4096);
    assert(gate_creates==10 && gate_destroys==10);
    puts("GFG output worker: deep immutable metadata, bounded jobs, timed drain and deferred errors PASS");
    return 0;
}

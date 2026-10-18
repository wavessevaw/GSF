#define _GNU_SOURCE
/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded asynchronous WSI output. App IDs/fences stay on the real frame.
 * Deep copies include every supported array and nested timing record. Unknown
 * metadata is synchronously forwarded after draining earlier output jobs.
 * Queue host serialization is supplied by the layer dispatch, not assumed.
 */
#include "output_worker.h"
#include "present_chain.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

typedef struct output_job {
    VkPresentInfoKHR pi;
    gfg_present_chain chain;
    VkSwapchainKHR swapchain;
    uint32_t image;
    VkSemaphore waits[16];
    VkFence fence;
    uint64_t id;
    VkPresentTimeGOOGLE time;
    VkPresentTimingInfoEXT timing;
    uint64_t queued_ns, admitted_ns, serial;
} output_job;

struct gfg_output {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    pthread_mutex_t *wsi;
    output_job *pending;
    int busy, stopping, joined;
    VkResult error;
    VkResult advisory;
    uint64_t completed_id, submitted, completed, stale, backpressure;
    uint64_t stale_after_ns;
    gfg_gpu_fg *gpu;
    VkQueue queue;
    PFN_vkQueuePresentKHR present;
    gfg_present_id_hooks ids;
    VkDevice device;
    VkSemaphore *gates;
    VkSemaphore *acquired;
    unsigned char *image_pending;
    uint32_t image_count;
    PFN_vkCreateSemaphore create_semaphore;
    PFN_vkDestroySemaphore destroy_semaphore;
    PFN_vkQueueSubmit submit;
};

static uint64_t tick_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static output_job *copy_job(const VkPresentInfoKHR *app, int mode) {
    if (!app || app->swapchainCount != 1 || !app->pSwapchains || !app->pImageIndices ||
        app->waitSemaphoreCount > 16 || (app->waitSemaphoreCount && !app->pWaitSemaphores)) return NULL;
    output_job *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    if (gfg_pc_prepare(&j->chain, app, mode)) { free(j); return NULL; }
    j->pi = *app; j->swapchain = app->pSwapchains[0]; j->image = app->pImageIndices[0];
    j->pi.pSwapchains = &j->swapchain; j->pi.pImageIndices = &j->image;
    j->pi.pResults = NULL; /* never retain or write the app's output pointer later */
    if (app->waitSemaphoreCount) memcpy(j->waits, app->pWaitSemaphores,
                                       app->waitSemaphoreCount * sizeof(*j->waits));
    j->pi.pWaitSemaphores = j->waits;
    j->id = j->chain.app_id;
    for (uint32_t k = 0; k < j->chain.count; ++k) {
        gfg_present_node *n = &j->chain.nodes[k];
        switch (n->base.sType) {
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
            n->id1.pPresentIds = &j->id; break;
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
            n->id2.pPresentIds = &j->id; break;
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT:
            j->fence = n->fence.pFences[0]; n->fence.pFences = &j->fence; break;
        case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE:
            if (n->times.pTimes) { j->time = n->times.pTimes[0]; n->times.pTimes = &j->time; }
            break;
        case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT:
            j->timing = n->timings.pTimingInfos[0];
            if (j->timing.pNext) { free(j); return NULL; }
            n->timings.pTimingInfos = &j->timing; break;
        default: break; /* mode array was already internalized; damage omitted */
        }
    }
    j->pi.pNext = gfg_pc_head(&j->chain); j->queued_ns = tick_ns();
    return j;
}

static void *output_main(void *arg) {
    gfg_output *o = arg;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        while (!o->pending && !o->stopping) pthread_cond_wait(&o->changed, &o->lock);
        if (!o->pending && o->stopping) { pthread_mutex_unlock(&o->lock); break; }
        output_job *j = o->pending; o->pending = NULL; o->busy = 1;
        pthread_cond_broadcast(&o->changed);
        pthread_mutex_unlock(&o->lock);
        /* Two real periods: preserve the real frame, but never synthesize a
         * stale pair into a growing output backlog. The mailbox cannot grow. */
        o->stale_after_ns=2*gfg_gpu_fg_source_period_ns(o->gpu);
        const int stale = tick_ns() - j->queued_ns > o->stale_after_ns;
        if (stale) gfg_gpu_fg_restart_history(o->gpu);
        gfg_gpu_fg_set_source(o->gpu,j->queued_ns,j->admitted_ns,j->serial);
        gfg_gpu_fg_set_output_pacing(o->gpu,1);
        VkResult r = gfg_gpu_fg_present(o->gpu, o->queue, &j->pi, o->present, &o->ids);
        gfg_gpu_fg_set_output_pacing(o->gpu,0);
        pthread_mutex_lock(&o->lock);
        if (r < 0 && o->error == VK_SUCCESS) o->error = r;
        if (r == VK_SUBOPTIMAL_KHR) o->advisory = r;
        if (r >= 0 && j->id > o->completed_id) o->completed_id = j->id;
        ++o->completed; o->stale += stale; o->busy = 0;
        o->image_pending[j->image]=0;
        free(j); pthread_cond_broadcast(&o->changed);
        pthread_mutex_unlock(&o->lock);
    }
    return NULL;
}

gfg_output *gfg_output_create(gfg_gpu_fg *gpu, VkQueue queue,
        PFN_vkQueuePresentKHR present, const gfg_present_id_hooks *ids, pthread_mutex_t *wsi,
        VkDevice device, PFN_vkGetDeviceProcAddr gdpa, uint32_t image_count, const VkSemaphore *acquired) {
    gfg_output *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->gpu = gpu; o->queue = queue; o->present = present; o->wsi = wsi;
    o->stale_after_ns=2*gfg_gpu_fg_source_period_ns(gpu);
    o->device=device; o->image_count=image_count;
    o->create_semaphore=(PFN_vkCreateSemaphore)gdpa(device,"vkCreateSemaphore");
    o->destroy_semaphore=(PFN_vkDestroySemaphore)gdpa(device,"vkDestroySemaphore");
    o->submit=(PFN_vkQueueSubmit)gdpa(device,"vkQueueSubmit");
    if (!o->create_semaphore || !o->destroy_semaphore || !o->submit || !image_count || image_count>64) { free(o); return NULL; }
    if (ids) o->ids = *ids;
    if (pthread_mutex_init(&o->lock, NULL)) { free(o); return NULL; }
    pthread_condattr_t attr;
    pthread_condattr_init(&attr); pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int r = pthread_cond_init(&o->changed, &attr); pthread_condattr_destroy(&attr);
    if (r) { pthread_mutex_destroy(&o->lock); free(o); return NULL; }
    o->gates=calloc(image_count,sizeof(*o->gates));
    o->acquired=calloc(image_count,sizeof(*o->acquired));
    o->image_pending=calloc(image_count,1);
    if (!o->gates || !o->acquired || !o->image_pending) goto fail;
    if(acquired) memcpy(o->acquired,acquired,image_count*sizeof(*acquired));
    const VkSemaphoreCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(uint32_t i=0;i<image_count;++i)
        if(o->create_semaphore(device,&ci,NULL,&o->gates[i])!=VK_SUCCESS) goto fail;
    gfg_gpu_fg_set_output_thread(gpu, wsi);
    if (pthread_create(&o->thread, NULL, output_main, o)) {
        gfg_gpu_fg_set_output_thread(gpu, NULL);
        goto fail;
    }
    fprintf(stderr, "[gfg-native] output-worker active queued_capacity=1 executing_capacity=1 stale_after_ms=%.3f\n",
        (double)o->stale_after_ns/1e6);
    return o;
fail:
    if(o->gates) for(uint32_t i=0;i<image_count;++i)
        if(o->gates[i]) o->destroy_semaphore(device,o->gates[i],NULL);
    free(o->gates);free(o->acquired);free(o->image_pending);
    pthread_cond_destroy(&o->changed);pthread_mutex_destroy(&o->lock);free(o);return NULL;
}

VkResult gfg_output_submit(gfg_output *o, const VkPresentInfoKHR *app, uint64_t source_ns, int *handled) {
    *handled = 0;
    output_job *j = copy_job(app, o->ids.mode);
    if (!j) return VK_SUCCESS; /* caller drains and forwards synchronously */
    if(source_ns)j->queued_ns=source_ns;
    if(j->image>=o->image_count){free(j);return VK_SUCCESS;}
    pthread_mutex_lock(&o->lock);
    if (o->pending) ++o->backpressure;
    while (o->pending && !o->stopping && !o->error) pthread_cond_wait(&o->changed, &o->lock);
    *handled = 1;
    VkResult r = o->error;
    if (o->stopping) r = VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_SUCCESS) {
        /* vkQueuePresent's waits must be SUBMITTED before returning to the
         * application. Otherwise a conforming app can reuse a render-finished
         * semaphore before our delayed wait exists. Consume them now, handing
         * the worker a private semaphore for this acquired real image.
         * Reacquisition of that same image orders reuse of its private gate. */
        VkPipelineStageFlags stages[16];
        for(uint32_t k=0;k<j->pi.waitSemaphoreCount;++k) stages[k]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo consume={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount=j->pi.waitSemaphoreCount,.pWaitSemaphores=j->pi.pWaitSemaphores,
            .pWaitDstStageMask=stages,.signalSemaphoreCount=1,.pSignalSemaphores=&o->gates[j->image]};
        r=o->submit(o->queue,1,&consume,VK_NULL_HANDLE);
        if(r==VK_SUCCESS){
            j->waits[0]=o->gates[j->image];j->pi.waitSemaphoreCount=1;
            j->admitted_ns=tick_ns(); /* capture time must survive mailbox backpressure */
            j->serial=o->submitted+1;
            o->image_pending[j->image]=1;
            o->pending = j; ++o->submitted; pthread_cond_broadcast(&o->changed); r = o->advisory;
        } else {o->error=r;free(j);}
    }
    else free(j);
    if (app->pResults) app->pResults[0] = r;
    pthread_mutex_unlock(&o->lock);
    return r;
}

VkResult gfg_output_drain(gfg_output *o, uint64_t id, uint64_t timeout, uint64_t *remaining) {
    const uint64_t start = tick_ns();
    const uint64_t deadline = timeout > UINT64_MAX - start ? UINT64_MAX : start + timeout;
    pthread_mutex_lock(&o->lock);
    VkResult r = VK_SUCCESS;
    while ((o->pending || o->busy) && (!id || o->completed_id < id)) {
        if (!timeout || (deadline != UINT64_MAX && tick_ns() >= deadline)) { r = VK_TIMEOUT; break; }
        if (deadline == UINT64_MAX) pthread_cond_wait(&o->changed, &o->lock);
        else {
            struct timespec end = {.tv_sec = (time_t)(deadline / 1000000000ull),
                                   .tv_nsec = (long)(deadline % 1000000000ull)};
            if (pthread_cond_timedwait(&o->changed, &o->lock, &end) == ETIMEDOUT) { r = VK_TIMEOUT; break; }
        }
    }
    if (r == VK_SUCCESS && o->error && (!id || o->completed_id < id)) r = o->error;
    pthread_mutex_unlock(&o->lock);
    if (remaining) {
        uint64_t elapsed = tick_ns() - start;
        *remaining = timeout == UINT64_MAX ? timeout : elapsed < timeout ? timeout - elapsed : 0;
    }
    return r;
}

pthread_mutex_t *gfg_output_wsi(gfg_output *o) { return o->wsi; }
int gfg_output_waiting_image(gfg_output *o) { return gfg_gpu_fg_waiting_image(o->gpu); }
VkResult gfg_output_status(gfg_output *o) {
    pthread_mutex_lock(&o->lock); VkResult r = o->error ? o->error : o->advisory;
    pthread_mutex_unlock(&o->lock); return r;
}
int gfg_output_sem_reusable(gfg_output *o,VkSemaphore semaphore) {
    if(!semaphore) return 1;
    int ready=1;pthread_mutex_lock(&o->lock);
    for(uint32_t i=0;i<o->image_count;++i)
        if(o->acquired[i]==semaphore && o->image_pending[i]) ready=0;
    pthread_mutex_unlock(&o->lock);return ready;
}
void gfg_output_acquired(gfg_output *o,uint32_t image,VkSemaphore semaphore) {
    pthread_mutex_lock(&o->lock);
    for(uint32_t i=0;i<o->image_count;++i) if(o->acquired[i]==semaphore) o->acquired[i]=VK_NULL_HANDLE;
    if(image<o->image_count) o->acquired[image]=semaphore;
    pthread_mutex_unlock(&o->lock);
}
void gfg_output_stop(gfg_output *o) {
    if (!o) return;
    if(o->joined) return;
    pthread_mutex_lock(&o->lock); o->stopping = 1;
    pthread_cond_broadcast(&o->changed); pthread_mutex_unlock(&o->lock);
    pthread_join(o->thread, NULL);
    o->joined=1;
    fprintf(stderr, "[gfg-native] output-worker retired submitted=%llu completed=%llu stale=%llu backpressure=%llu error=%d\n",
        (unsigned long long)o->submitted, (unsigned long long)o->completed,
        (unsigned long long)o->stale, (unsigned long long)o->backpressure, (int)o->error);
}
void gfg_output_destroy(gfg_output *o) {
    if(!o) return;
    gfg_output_stop(o);
    for(uint32_t i=0;i<o->image_count;++i) o->destroy_semaphore(o->device,o->gates[i],NULL);
    free(o->gates);
    free(o->acquired);free(o->image_pending);
    pthread_cond_destroy(&o->changed); pthread_mutex_destroy(&o->lock); free(o);
}

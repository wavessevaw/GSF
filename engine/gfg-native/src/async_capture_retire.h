/* SPDX-License-Identifier: GPL-3.0-or-later
 * EXPERIMENTAL: asynchronous GPU snapshot retirement for virtual WSI.
 *
 * App vkQueuePresentKHR callback only records/submits GPU copy and enqueues
 * its fence. A dedicated worker waits for fence completion, then retires
 * the app image. This helper does NOT own display WSI or interpolate frames.
 * The queue is externally serialized by the caller's Vulkan host policy.
 *
 * IMPORTANT: only use after proving the GPU copy and history buffers are
 * safe from overwrite until the output consumer is finished. A real game
 * requires its own queue and output resource lifetime arbitration.
 */
#ifndef GFG_ASYNC_CAPTURE_RETIRE_H
#define GFG_ASYNC_CAPTURE_RETIRE_H
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "virtual_wsi_facade.h"

typedef void (*gfg_async_copy_record)(void *user,VkCommandBuffer cmd,uint64_t ticket);
/* Optional second stage: one output WSI copy/present, owned by retirement
 * worker. Source slot stays unavailable until this callback completes. */
typedef VkResult (*gfg_async_copy_complete)(void *user,uint32_t image,uint64_t ticket);

typedef struct {
    VkCommandBuffer command;
    VkFence fence;
    uint64_t ticket;
    int busy;
} gfg_async_capture_slot;

typedef struct {
    gfg_virtual_wsi *virtual_app;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    gfg_async_copy_record record;
    gfg_async_copy_complete complete;
    void *user;
    uint32_t count;
    gfg_async_capture_slot slots[GFG_VIRTUAL_MAX_IMAGES];
    uint32_t pending[GFG_VIRTUAL_MAX_IMAGES],head,tail,queued;
    uint64_t submitted,retired,worker_gpu_waits,submit_failures;
    uint64_t app_gpu_waits; /* contractual invariant: always zero */
    uint32_t test_delayed_retire_ms; /* fault injection, not normal pacing */
    VkResult failure;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t worker;
    int worker_started,stop,initialized;
} gfg_async_capture;

static inline void *gfg_async_capture_worker(void *opaque) {
    gfg_async_capture *a=opaque;
    pthread_mutex_lock(&a->lock);
    for(;;) {
        while(!a->queued && !a->stop)
            pthread_cond_wait(&a->changed,&a->lock);
        if(!a->queued)break;
        const uint32_t idx=a->pending[a->head];
        a->head=(a->head+1)%a->count;
        --a->queued;
        const VkFence fence=a->slots[idx].fence;
        const uint64_t ticket=a->slots[idx].ticket;
        ++a->worker_gpu_waits;
        pthread_mutex_unlock(&a->lock);
        /* Only the retirement worker blocks on GPU completion. */
        VkResult result=vkWaitForFences(a->device,1,&fence,VK_TRUE,UINT64_MAX);
        if(result==VK_SUCCESS && a->test_delayed_retire_ms) {
            const struct timespec delay={
                .tv_sec=(time_t)(a->test_delayed_retire_ms/1000u),
                .tv_nsec=(long)(a->test_delayed_retire_ms%1000u)*1000000L
            };
            nanosleep(&delay,NULL);
        }
        if(result==VK_SUCCESS && a->complete)
            result=a->complete(a->user,idx,ticket);
        if(result==VK_SUCCESS)
            result=gfg_vwsi_retire(a->virtual_app,idx,ticket,1);
        pthread_mutex_lock(&a->lock);
        if(result!=VK_SUCCESS && a->failure==VK_SUCCESS)
            a->failure=result;
        if(result==VK_SUCCESS)++a->retired;
        /* Even on error, don't let the app reuse the source: virtual WSI
         * remains COPYING unless the copy fence completed successfully. */
        a->slots[idx].busy=0;
        pthread_cond_broadcast(&a->changed);
        if(result!=VK_SUCCESS) {
            a->stop=1;
            break;
        }
    }
    pthread_cond_broadcast(&a->changed);
    pthread_mutex_unlock(&a->lock);
    return NULL;
}

static inline void gfg_async_capture_cleanup(gfg_async_capture *a) {
    if(!a || !a->initialized)return;
    pthread_mutex_lock(&a->lock);
    a->stop=1;
    pthread_cond_broadcast(&a->changed);
    pthread_mutex_unlock(&a->lock);
    if(a->worker_started) {
        pthread_join(a->worker,NULL);
        a->worker_started=0;
    }
    /* All active GPU work must be retired, or device already idle, before
     * destroying command buffers and fences. Call only during test teardown. */
    for(uint32_t i=0;i<a->count;i++)
        if(a->slots[i].fence)
            vkDestroyFence(a->device,a->slots[i].fence,NULL);
    if(a->pool)vkDestroyCommandPool(a->device,a->pool,NULL);
    pthread_cond_destroy(&a->changed);
    pthread_mutex_destroy(&a->lock);
    a->initialized=0;
}

static inline VkResult gfg_async_capture_init(gfg_async_capture *a,
            gfg_virtual_wsi *virtual_app,VkDevice device,VkQueue queue,
            uint32_t family,gfg_async_copy_record record,void *user) {
    if(!a||!virtual_app||!device||!queue||!record||
       !virtual_app->source.count ||
       virtual_app->source.count>GFG_VIRTUAL_MAX_IMAGES)
        return VK_ERROR_INITIALIZATION_FAILED;
    memset(a,0,sizeof(*a));
    a->virtual_app=virtual_app;
    a->device=device;a->queue=queue;a->record=record;a->user=user;
    a->count=virtual_app->source.count;
    a->failure=VK_SUCCESS;
    if(pthread_mutex_init(&a->lock,NULL))return VK_ERROR_INITIALIZATION_FAILED;
    if(pthread_cond_init(&a->changed,NULL)) {
        pthread_mutex_destroy(&a->lock);return VK_ERROR_INITIALIZATION_FAILED;
    }
    a->initialized=1;
    VkCommandPoolCreateInfo pool_info={
        .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex=family};
    VkResult result=vkCreateCommandPool(device,&pool_info,NULL,&a->pool);
    if(result!=VK_SUCCESS)goto fail;
    VkCommandBuffer command[GFG_VIRTUAL_MAX_IMAGES]={0};
    VkCommandBufferAllocateInfo alloc={
        .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=a->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount=a->count};
    result=vkAllocateCommandBuffers(device,&alloc,command);
    if(result!=VK_SUCCESS)goto fail;
    for(uint32_t i=0;i<a->count;i++) {
        a->slots[i].command=command[i];
        const VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result=vkCreateFence(device,&fc,NULL,&a->slots[i].fence);
        if(result!=VK_SUCCESS)goto fail;
    }
    if(pthread_create(&a->worker,NULL,gfg_async_capture_worker,a)) {
        result=VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    a->worker_started=1;
    return VK_SUCCESS;
fail:
    gfg_async_capture_cleanup(a);
    return result;
}

/* Callback for gfg_virtual_wsi.copy_submit. No VkWaitForFences here.
 * The GPU queue consumes app render waits exactly once. This callback is
 * test-scoped, and its source history recorder must be thread-safe with the
 * output consumer; the corresponding per-source slot is not reused before
 * the worker retires it. */
static inline VkResult gfg_async_capture_submit(void *opaque,
        uint32_t image,uint64_t ticket,uint32_t wait_count,
        const VkSemaphore *waits) {
    gfg_async_capture *a=opaque;
    if(!a||!a->initialized||image>=a->count||!ticket||
       wait_count>16||(wait_count&&!waits))
        return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&a->lock);
    if(a->failure!=VK_SUCCESS||a->stop||
       a->slots[image].busy||a->queued>=a->count) {
        VkResult error=a->failure!=VK_SUCCESS?a->failure:VK_NOT_READY;
        pthread_mutex_unlock(&a->lock);
        return error;
    }
    gfg_async_capture_slot *slot=&a->slots[image];
    VkResult result=vkResetCommandBuffer(slot->command,0);
    if(result!=VK_SUCCESS)goto bad;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    result=vkBeginCommandBuffer(slot->command,&begin);
    if(result!=VK_SUCCESS)goto bad;
    a->record(a->user,slot->command,ticket);
    result=vkEndCommandBuffer(slot->command);
    if(result!=VK_SUCCESS)goto bad;
    result=vkResetFences(a->device,1,&slot->fence);
    if(result!=VK_SUCCESS)goto bad;
    VkPipelineStageFlags stages[16]={0};
    for(uint32_t j=0;j<wait_count;j++)stages[j]=VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount=wait_count,.pWaitSemaphores=waits,
        .pWaitDstStageMask=wait_count?stages:NULL,
        .commandBufferCount=1,.pCommandBuffers=&slot->command};
    result=vkQueueSubmit(a->queue,1,&submit,slot->fence);
    if(result!=VK_SUCCESS)goto bad;
    slot->ticket=ticket;slot->busy=1;
    a->pending[a->tail]=image;
    a->tail=(a->tail+1)%a->count;
    ++a->queued;++a->submitted;
    pthread_cond_broadcast(&a->changed);
    pthread_mutex_unlock(&a->lock);
    return VK_SUCCESS;
bad:
    ++a->submit_failures;
    a->failure=result;
    a->stop=1;
    pthread_cond_broadcast(&a->changed);
    pthread_mutex_unlock(&a->lock);
    return result;
}

/* The output test may wait for its history snapshot on CPU for pixel
 * readback. This is NOT in the application's vkQueuePresentKHR callback.
 * The production output owner must replace this with GPU-side dependency. */
static inline VkResult gfg_async_capture_wait_ticket(
        gfg_async_capture *a,uint64_t ticket) {
    if(!a || !a->initialized || !ticket)return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&a->lock);
    while(a->retired<ticket && a->failure==VK_SUCCESS && !a->stop)
        pthread_cond_wait(&a->changed,&a->lock);
    VkResult r=a->failure!=VK_SUCCESS?a->failure:
        (a->retired>=ticket?VK_SUCCESS:VK_ERROR_DEVICE_LOST);
    pthread_mutex_unlock(&a->lock);
    return r;
}
#endif

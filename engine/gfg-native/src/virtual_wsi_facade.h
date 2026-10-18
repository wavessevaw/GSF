/* SPDX-License-Identifier: GPL-3.0-or-later
 * Vulkan WSI-shaped source proxy for the experimental single-output-owner.
 *
 * This is NOT an enabled Vulkan layer. It models the app-facing contract
 * independently of the visible compositor swapchain. Vulkan images belong
 * to the caller. A real layer must also intercept every applicable WSI
 * query/extension and implement format/pNext/oldSwapchain compatibility.
 */
#ifndef GFG_VIRTUAL_WSI_FACADE_H
#define GFG_VIRTUAL_WSI_FACADE_H
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include "virtual_source.h"

typedef VkResult (*gfg_vwsi_acquire_signal)(void *, VkSemaphore, VkFence);
typedef VkResult (*gfg_vwsi_copy_submit)(void *, uint32_t, uint64_t,
                        uint32_t, const VkSemaphore *);
typedef struct {
    gfg_virtual_source source;
    VkSwapchainKHR handle;
    VkImage images[GFG_VIRTUAL_MAX_IMAGES];
    VkFormat format;
    VkExtent2D extent;
    uint32_t accepts,copy_retirements,incorrect;
    void *user;
    gfg_vwsi_acquire_signal signal_acquire;
    gfg_vwsi_copy_submit copy_submit;
    int closed;
    pthread_mutex_t mutex;     /* protects virtual source slots + callbacks' status */
    pthread_cond_t released;   /* CLOCK_MONOTONIC: wake acquire on copy retirement */
} gfg_virtual_wsi;

static inline VkResult gfg_vwsi_init(gfg_virtual_wsi *v,
                VkSwapchainKHR opaque_handle,const VkImage *images,
                uint32_t count,VkFormat format,VkExtent2D extent,
                void *user,gfg_vwsi_acquire_signal acquire_fn,
                gfg_vwsi_copy_submit copy_fn) {
    if(!v || !opaque_handle || !images || !count ||
       count>GFG_VIRTUAL_MAX_IMAGES || !extent.width || !extent.height ||
       !acquire_fn || !copy_fn)
        return VK_ERROR_INITIALIZATION_FAILED;
    memset(v,0,sizeof(*v));
    if(!gfg_virtual_init(&v->source,count))return VK_ERROR_INITIALIZATION_FAILED;
    for(uint32_t i=0;i<count;++i){
        if(!images[i])return VK_ERROR_INITIALIZATION_FAILED;
        v->images[i]=images[i];
    }
    v->handle=opaque_handle;v->format=format;v->extent=extent;
    v->user=user;v->signal_acquire=acquire_fn;v->copy_submit=copy_fn;
    if(pthread_mutex_init(&v->mutex,NULL)!=0)return VK_ERROR_INITIALIZATION_FAILED;
    pthread_condattr_t attr;
    if(pthread_condattr_init(&attr)!=0) {
        pthread_mutex_destroy(&v->mutex);return VK_ERROR_INITIALIZATION_FAILED;
    }
    const int clock_status=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    const int cond_status=clock_status?clock_status:
        pthread_cond_init(&v->released,&attr);
    pthread_condattr_destroy(&attr);
    if(cond_status!=0) {
        pthread_mutex_destroy(&v->mutex);return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}
static inline VkResult gfg_vwsi_get_images(gfg_virtual_wsi *v,
                 VkSwapchainKHR swapchain,uint32_t *count,VkImage *images) {
    if(!v||!count)return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&v->mutex);
    if(v->closed||swapchain!=v->handle) {
        pthread_mutex_unlock(&v->mutex);return VK_ERROR_OUT_OF_DATE_KHR;
    }
    const uint32_t available=v->source.count;
    if(!images) {
        *count=available;
        pthread_mutex_unlock(&v->mutex);
        return VK_SUCCESS;
    }
    uint32_t provided=*count;
    uint32_t n=provided<available?provided:available;
    memcpy(images,v->images,(size_t)n*sizeof(VkImage));
    *count=n;
    pthread_mutex_unlock(&v->mutex);
    return provided<available?VK_INCOMPLETE:VK_SUCCESS;
}
/* An app's infinite Acquire must BLOCK until its image can be reused.
 * Caller MUST run a GPU copy-retirement worker that signals released;
 * waiting without a separate retirement producer would deadlock.
 * Monotonic absolute deadlines match the VK_TIMEOUT contract. */
static inline VkResult gfg_vwsi_acquire(gfg_virtual_wsi *v,
           VkSwapchainKHR swapchain,uint64_t timeout,
           VkSemaphore semaphore,VkFence fence,uint32_t *index) {
    if(!v||!index)return VK_ERROR_INITIALIZATION_FAILED;
    if(!semaphore && !fence)return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&v->mutex);
    if(v->closed||swapchain!=v->handle) {
        pthread_mutex_unlock(&v->mutex);return VK_ERROR_OUT_OF_DATE_KHR;
    }
    struct timespec until={0};
    if(timeout && timeout!=UINT64_MAX) {
        clock_gettime(CLOCK_MONOTONIC,&until);
        until.tv_sec+=(time_t)(timeout/1000000000ull);
        until.tv_nsec+=(long)(timeout%1000000000ull);
        if(until.tv_nsec>=1000000000L) {
            ++until.tv_sec;until.tv_nsec-=1000000000L;
        }
    }
    unsigned idx=0;
    for(;;) {
        if(v->closed) {
            pthread_mutex_unlock(&v->mutex);return VK_ERROR_OUT_OF_DATE_KHR;
        }
        if(gfg_virtual_acquire(&v->source,&idx))break;
        if(!timeout) {
            pthread_mutex_unlock(&v->mutex);return VK_NOT_READY;
        }
        int rc=timeout==UINT64_MAX?
            pthread_cond_wait(&v->released,&v->mutex):
            pthread_cond_timedwait(&v->released,&v->mutex,&until);
        if(rc==ETIMEDOUT) {
            pthread_mutex_unlock(&v->mutex);return VK_TIMEOUT;
        }
        if(rc!=0) {
            pthread_mutex_unlock(&v->mutex);return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    pthread_mutex_unlock(&v->mutex);
    /* GPU queue signals app's acquire semaphore/fence. Revert on failure. */
    VkResult result=v->signal_acquire(v->user,semaphore,fence);
    if(result!=VK_SUCCESS) {
        pthread_mutex_lock(&v->mutex);
        gfg_virtual_cancel_acquire(&v->source,idx);
        pthread_cond_broadcast(&v->released);
        pthread_mutex_unlock(&v->mutex);
        return result;
    }
    *index=idx;return VK_SUCCESS;
}
/* vkAcquireNextImage2KHR is used by some Vulkan games and device-group
 * wrappers. This single-GPU prototype accepts only deviceMask == 1 and
 * no opaque pNext; other requests must be handled by the original WSI. */
static inline VkResult gfg_vwsi_acquire2(gfg_virtual_wsi *v,
        const VkAcquireNextImageInfoKHR *info,uint32_t *index) {
    if(!info || info->sType!=VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR ||
       info->pNext || info->deviceMask!=1u)
        return VK_ERROR_INITIALIZATION_FAILED;
    return gfg_vwsi_acquire(v,info->swapchain,info->timeout,
                            info->semaphore,info->fence,index);
}

/* Present is redirected to the CAPTURE queue, NOT the output swapchain.
 * The callback MUST submit a GPU copy waiting on the game's real render
 * semaphore(s). A successful callback owns those waits exactly once and
 * signals a copy fence. Release is permitted only after fence completion.
 * No second vkQueuePresentKHR may be issued for this source image.
 *
 * Subset: one virtual swapchain, no pNext (present id/fence, mode, etc.).
 * Fail closed BEFORE consuming semaphores on unknown extensions. */
static inline VkResult gfg_vwsi_present(gfg_virtual_wsi *v,
                     const VkPresentInfoKHR *info,uint64_t *ticket_out) {
    if(!v||!info||!ticket_out ||
       info->sType!=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR ||
       info->pNext || info->swapchainCount!=1 ||
       !info->pSwapchains || !info->pImageIndices ||
       info->pSwapchains[0]!=v->handle ||
       (info->waitSemaphoreCount && !info->pWaitSemaphores))
        return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t idx=info->pImageIndices[0];
    uint64_t ticket=0;
    pthread_mutex_lock(&v->mutex);
    if(v->closed) {
        pthread_mutex_unlock(&v->mutex);return VK_ERROR_OUT_OF_DATE_KHR;
    }
    const int accepted=gfg_virtual_begin_snapshot(&v->source,idx,&ticket);
    if(!accepted)++v->incorrect;
    pthread_mutex_unlock(&v->mutex);
    if(!accepted) {
        if(info->pResults)info->pResults[0]=VK_ERROR_INITIALIZATION_FAILED;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* After submitting a copy, the callback takes irrevocable ownership
     * of app's render waits. On failure leave the slot COPYING/quarantined:
     * replaying a binary wait could corrupt the queue. */
    VkResult r=v->copy_submit(v->user,idx,ticket,
                             info->waitSemaphoreCount,info->pWaitSemaphores);
    pthread_mutex_lock(&v->mutex);
    if(r!=VK_SUCCESS)++v->incorrect;
    else ++v->accepts;
    pthread_mutex_unlock(&v->mutex);
    if(info->pResults)info->pResults[0]=r;
    if(r!=VK_SUCCESS)return r;
    *ticket_out=ticket;
    return VK_SUCCESS;
}
static inline VkResult gfg_vwsi_retire(gfg_virtual_wsi *v,
               uint32_t image,uint64_t ticket,int copy_fence_signalled) {
    if(!v)return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&v->mutex);
    if(v->closed) {
        pthread_mutex_unlock(&v->mutex);return VK_ERROR_OUT_OF_DATE_KHR;
    }
    const int released=gfg_virtual_release(&v->source,image,ticket,copy_fence_signalled);
    if(!released) {
        pthread_mutex_unlock(&v->mutex);
        return copy_fence_signalled?VK_ERROR_INITIALIZATION_FAILED:VK_NOT_READY;
    }
    ++v->copy_retirements;
    pthread_cond_broadcast(&v->released);
    pthread_mutex_unlock(&v->mutex);
    return VK_SUCCESS;
}
static inline void gfg_vwsi_close(gfg_virtual_wsi *v) {
    if(!v)return;
    pthread_mutex_lock(&v->mutex);
    v->closed=1;
    gfg_virtual_close(&v->source);
    pthread_cond_broadcast(&v->released);
    pthread_mutex_unlock(&v->mutex);
}
/* Only after all acquire/present callers and the copy-retirement worker
 * have joined. No OS synchronization objects may be freed while in use. */
static inline void gfg_vwsi_fini(gfg_virtual_wsi *v) {
    if(!v)return;
    pthread_cond_destroy(&v->released);
    pthread_mutex_destroy(&v->mutex);
}
#endif

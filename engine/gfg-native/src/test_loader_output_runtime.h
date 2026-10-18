/* SPDX-License-Identifier: GPL-3.0-or-later
 * Isolated TEST-ONLY Vulkan-Loader runtime for one live virtual game WSI.
 * Real GPU image capture -> independent history -> ONE compositor output WSI.
 * Optional GPU-built A/B diagnostic intermediate, NOT optical flow or Deck.
 *
 * Calls global Vulkan entrypoints only in this isolated explicit-layer
 * executable. Normal GFG production layer never includes this header.
 */
#ifndef GFG_TEST_LOADER_OUTPUT_RUNTIME_H
#define GFG_TEST_LOADER_OUTPUT_RUNTIME_H
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "output_proxy_bootstrap.h"
#include "async_capture_retire.h"
#include "output_frame_pacer.h"

typedef struct {
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    gfg_output_proxy *proxy;
    gfg_offscreen_source_pool history;
    gfg_offscreen_dispatch memory_ops;
    gfg_async_capture capture;
    VkImage output_images[GFG_OFFSCREEN_MAX_IMAGES];
    VkSemaphore ready[GFG_OFFSCREEN_MAX_IMAGES];
    uint8_t seen[GFG_OFFSCREEN_MAX_IMAGES];
    uint32_t output_count;
    VkSemaphore acquired;
    VkCommandPool output_pool;
    VkCommandBuffer output_cmd;
    VkFence output_fence;
    uint64_t output_presents;
    uint64_t output_originals, output_intermediates;
    gfg_output_frame_pacer output_pacer;
    uint32_t last_history_image;
    int have_previous, interleave;
    int started;
} gfg_test_loader_runtime;

static inline VkImageMemoryBarrier gfg_test_barrier(VkImage image,
        VkImageLayout old_layout,VkImageLayout new_layout,
        VkAccessFlags from,VkAccessFlags to) {
    const VkImageMemoryBarrier b={
        .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=old_layout,.newLayout=new_layout,
        .srcAccessMask=from,.dstAccessMask=to,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}
    };
    return b;
}

static inline VkResult gfg_test_live_acquire(void *user,
        VkSemaphore semaphore,VkFence fence) {
    gfg_test_loader_runtime *r=user;
    if(!r||!r->queue)return VK_ERROR_INITIALIZATION_FAILED;
    const VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount=semaphore?1u:0u,
        .pSignalSemaphores=semaphore?&semaphore:NULL};
    /* GPU-signals acquire; never signal a binary VkSemaphore on host. */
    return vkQueueSubmit(r->queue,1,&submit,fence);
}

static inline VkResult gfg_test_live_capture(void *user,
        uint32_t image,uint64_t ticket,uint32_t count,
        const VkSemaphore *waits) {
    gfg_test_loader_runtime *r=user;
    if(!r)return VK_ERROR_INITIALIZATION_FAILED;
    return gfg_async_capture_submit(&r->capture,image,ticket,count,waits);
}

static inline void gfg_test_live_record(void *user,
        VkCommandBuffer cmd,uint64_t ticket) {
    gfg_test_loader_runtime *r=user;
    const uint32_t idx=(uint32_t)(ticket%r->history.count);
    const VkImage src=r->proxy->source.images[idx];
    const VkImage dst=r->history.images[idx];
    /* Only loader test app with virtual PRESENT->TRANSFER_SRC barrier
     * rewriting is eligible. Source never goes to real WSI. */
    const VkImageMemoryBarrier pre=gfg_test_barrier(dst,
        ticket>r->history.count?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                                VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        ticket>r->history.count?VK_ACCESS_TRANSFER_READ_BIT:0,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd,
        ticket>r->history.count?VK_PIPELINE_STAGE_TRANSFER_BIT:
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&pre);
    const VkImageCopy copy={
        .srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .extent={r->proxy->source.extent.width,r->proxy->source.extent.height,1}
    };
    vkCmdCopyImage(cmd,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    const VkImageMemoryBarrier post=gfg_test_barrier(dst,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&post);
}

/* Worker only; never on the application's vkQueuePresentKHR call stack.
 * Retire source AFTER recording/submitting the sole output and handing it
 * to the real WSI. The per-app-index history slot is therefore safe to
 * overwrite after this callback returns, on this same Vulkan queue. */
static inline VkResult gfg_test_live_emit(gfg_test_loader_runtime *r,
        uint32_t image,uint32_t previous,int intermediate) {
    if(!r||!r->started||image>=r->history.count||
       (intermediate && previous>=r->history.count))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result=vkWaitForFences(r->device,1,&r->output_fence,
                                   VK_TRUE,UINT64_MAX);
    if(result!=VK_SUCCESS)return result;
    uint32_t out_idx=UINT32_MAX;
    result=vkAcquireNextImageKHR(r->device,r->proxy->output,
                 2000000000ull,r->acquired,VK_NULL_HANDLE,&out_idx);
    if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR)return result;
    if(out_idx>=r->output_count)return VK_ERROR_INITIALIZATION_FAILED;
    result=vkResetCommandBuffer(r->output_cmd,0);
    if(result!=VK_SUCCESS)return result;
    const VkCommandBufferBeginInfo begin={
        .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    result=vkBeginCommandBuffer(r->output_cmd,&begin);
    if(result!=VK_SUCCESS)return result;
    const VkImageMemoryBarrier pre=gfg_test_barrier(r->output_images[out_idx],
        r->seen[out_idx]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                         VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(r->output_cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&pre);
    const uint32_t width=r->proxy->source.extent.width;
    const uint32_t height=r->proxy->source.extent.height;
    if(intermediate) {
        /* One output VkImage composed from two *distinct* GPU history
         * images. Left half is preceding A, right half is current B.
         * This verifies insertion only; it is NOT optical-flow output. */
        const VkImageCopy left={
            .srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .extent={width/2,height,1}
        };
        VkImageCopy right=left;
        right.srcOffset.x=(int32_t)(width/2);
        right.dstOffset.x=(int32_t)(width/2);
        vkCmdCopyImage(r->output_cmd,r->history.images[previous],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->output_images[out_idx],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&left);
        vkCmdCopyImage(r->output_cmd,r->history.images[image],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->output_images[out_idx],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&right);
    } else {
        const VkImageCopy region={
            .srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .extent={width,height,1}
        };
        vkCmdCopyImage(r->output_cmd,r->history.images[image],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->output_images[out_idx],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    }
    const VkImageMemoryBarrier post=gfg_test_barrier(r->output_images[out_idx],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_TRANSFER_WRITE_BIT,0);
    vkCmdPipelineBarrier(r->output_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&post);
    result=vkEndCommandBuffer(r->output_cmd);
    if(result!=VK_SUCCESS)return result;
    result=vkResetFences(r->device,1,&r->output_fence);
    if(result!=VK_SUCCESS)return result;
    const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit={
        .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount=1,.pWaitSemaphores=&r->acquired,
        .pWaitDstStageMask=&stage,.commandBufferCount=1,
        .pCommandBuffers=&r->output_cmd,
        .signalSemaphoreCount=1,.pSignalSemaphores=&r->ready[out_idx]
    };
    result=vkQueueSubmit(r->queue,1,&submit,r->output_fence);
    if(result!=VK_SUCCESS)return result;
    /* Pace the output WORKER, not the game thread. Output frame spacing
     * uses an absolute monotonic target. When the source arrives late,
     * reset from actual submission, do not issue zero-gap catch-up frames. */
    if(r->interleave && gfg_output_pacer_before_present(&r->output_pacer))
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkPresentInfoKHR present={
        .sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount=1,.pWaitSemaphores=&r->ready[out_idx],
        .swapchainCount=1,.pSwapchains=&r->proxy->output,
        .pImageIndices=&out_idx
    };
    result=vkQueuePresentKHR(r->queue,&present);
    if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR)return result;
    r->seen[out_idx]=1;
    ++r->output_presents;
    if(intermediate)++r->output_intermediates;
    else ++r->output_originals;
    fprintf(stderr,"GSF_LOADER_LIVE %s output_present=%llu originals=%llu intermediate=%llu one_real_wsi=1 worker=1\n",
        intermediate?"AB_SPLIT":"ORIGINAL",
        (unsigned long long)r->output_presents,
        (unsigned long long)r->output_originals,
        (unsigned long long)r->output_intermediates);
    return VK_SUCCESS;
}

/* GPU-copy retirement worker owns output presentation. Once B arrives,
 * output A/B marked composite BEFORE original B. Initial A is unmodified.
 * The pause is deliberately test-only pacing to make X11 observation
 * deterministic; real Deck pacing must use compositor feedback. */
static inline VkResult gfg_test_live_output(void *user,
        uint32_t image,uint64_t ticket) {
    gfg_test_loader_runtime *r=user;
    if(!r||!r->started||!ticket||image!=ticket%r->history.count)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(r->interleave && r->have_previous) {
        VkResult result=gfg_test_live_emit(r,image,r->last_history_image,1);
        if(result!=VK_SUCCESS)return result;
        /* No fixed 35ms diagnostic exposure. The actual worker pacer
         * spaces BOTH A/B and B output by the same target interval. */
    }
    VkResult result=gfg_test_live_emit(r,image,0,0);
    if(result!=VK_SUCCESS)return result;
    r->last_history_image=image;
    r->have_previous=1;
    return VK_SUCCESS;
}
/* Called after gfg_output_proxy_create, when both source and private output
 * swapchains exist. Test-only and intentionally supports only one graphics
 * + presentation queue family. */
static inline VkResult gfg_test_live_init(gfg_test_loader_runtime *r,
        VkDevice device,VkQueue queue,uint32_t family,
        gfg_output_proxy *proxy,gfg_offscreen_dispatch memory_ops,
        const VkSwapchainCreateInfoKHR *original) {
    if(!r||!device||!queue||!proxy||!proxy->app_initialized||!original)
        return VK_ERROR_INITIALIZATION_FAILED;
    r->device=device;r->queue=queue;r->family=family;r->proxy=proxy;
    r->memory_ops=memory_ops;
    r->interleave=getenv("GFG_NATIVE_TEST_OUTPUT_INTERLEAVE")!=NULL;
    if(r->interleave)gfg_output_pacer_init(&r->output_pacer,
        getenv("GFG_NATIVE_TEST_PACED_45_90")?90u:60u);
    if(r->interleave && (proxy->source.extent.width<2 ||
                        (proxy->source.extent.width&1u)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkSwapchainCreateInfoKHR history_ci=*original;
    history_ci.imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult result=gfg_offscreen_pool_create(&r->history,device,
        proxy->physical,&history_ci,&memory_ops);
    if(result!=VK_SUCCESS)return result;
    result=vkGetSwapchainImagesKHR(device,proxy->output,
                                  &r->output_count,NULL);
    if(result!=VK_SUCCESS)return result;
    if(!r->output_count||r->output_count>GFG_OFFSCREEN_MAX_IMAGES)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t count=r->output_count;
    result=vkGetSwapchainImagesKHR(device,proxy->output,
                                  &count,r->output_images);
    if(result!=VK_SUCCESS||count!=r->output_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkSemaphoreCreateInfo sem={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    result=vkCreateSemaphore(device,&sem,NULL,&r->acquired);
    if(result!=VK_SUCCESS)return result;
    for(uint32_t i=0;i<r->output_count;i++) {
        result=vkCreateSemaphore(device,&sem,NULL,&r->ready[i]);
        if(result!=VK_SUCCESS)return result;
    }
    const VkCommandPoolCreateInfo pool={
        .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex=family};
    result=vkCreateCommandPool(device,&pool,NULL,&r->output_pool);
    if(result!=VK_SUCCESS)return result;
    const VkCommandBufferAllocateInfo alloc={
        .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=r->output_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount=1};
    result=vkAllocateCommandBuffers(device,&alloc,&r->output_cmd);
    if(result!=VK_SUCCESS)return result;
    const VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags=VK_FENCE_CREATE_SIGNALED_BIT};
    result=vkCreateFence(device,&fc,NULL,&r->output_fence);
    if(result!=VK_SUCCESS)return result;
    /* All resources above established before worker can call output. */
    r->started=1;
    result=gfg_async_capture_init(&r->capture,&proxy->app,device,queue,
        family,gfg_test_live_record,r);
    if(result!=VK_SUCCESS){r->started=0;return result;}
    r->capture.complete=gfg_test_live_output;
    return VK_SUCCESS;
}
static inline void gfg_test_live_destroy(gfg_test_loader_runtime *r) {
    if(!r||!r->device)return;
    /* Worker first; in-flight capture and output WSI jobs must be retired
     * before either command pool or GPU-owned source/history images die. */
    gfg_async_capture_cleanup(&r->capture);
    if(r->interleave && r->output_pacer.emitted) {
        const uint64_t gaps=r->output_pacer.emitted-1u;
        fprintf(stderr,
            "GSF_OUTPUT_PACER rate=%llu emitted=%llu originals=%llu intermediate=%llu min_gap_us=%llu max_gap_us=%llu mean_gap_us=%llu late_reanchors=%llu worker_only=1\n",
            (unsigned long long)(r->output_pacer.target_ns?
                1000000000ull/r->output_pacer.target_ns:0),
            (unsigned long long)r->output_pacer.emitted,
            (unsigned long long)r->output_originals,
            (unsigned long long)r->output_intermediates,
            (unsigned long long)(r->output_pacer.gap_min_ns/1000u),
            (unsigned long long)(r->output_pacer.gap_max_ns/1000u),
            (unsigned long long)(gaps?r->output_pacer.gap_sum_ns/gaps/1000u:0),
            (unsigned long long)r->output_pacer.reanchors);
    }
    vkDeviceWaitIdle(r->device); /* teardown only */
    if(r->output_fence)vkDestroyFence(r->device,r->output_fence,NULL);
    if(r->output_pool)vkDestroyCommandPool(r->device,r->output_pool,NULL);
    for(uint32_t i=0;i<GFG_OFFSCREEN_MAX_IMAGES;i++)
        if(r->ready[i])vkDestroySemaphore(r->device,r->ready[i],NULL);
    if(r->acquired)vkDestroySemaphore(r->device,r->acquired,NULL);
    gfg_offscreen_pool_destroy(&r->history,r->device,&r->memory_ops);
    r->device=VK_NULL_HANDLE;r->started=0;
}
#endif

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
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
#include "te_engine.h"
#include "post_deadline_budget.h"
#endif

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
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
    VkInstance temporal_instance;
    PFN_vkGetInstanceProcAddr temporal_gipa;
    PFN_vkGetDeviceProcAddr temporal_gdpa;
    PFN_vkSetDeviceLoaderData temporal_loader;
    te_device *temporal_device;
    te_context *temporal;
    unsigned temporal_slot,temporal_multiplier,temporal_active_outputs;
    uint64_t post_max_age_ns,post_skipped_pairs;
    uint64_t captured_ns[GFG_VIRTUAL_MAX_IMAGES];
    unsigned temporal_outputs,temporal_slot_ids[3];
    float temporal_tau[3];
    VkBuffer verify_buffer;
    VkDeviceMemory verify_memory;
    unsigned char *verify_map;
    VkDeviceSize verify_image_bytes;
    uint64_t verify_outputs;
#endif
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
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
    if(image<GFG_VIRTUAL_MAX_IMAGES)
        r->captured_ns[image]=gfg_pacer_monotonic_ns();
#endif
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
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
    if(intermediate && r->temporal) {
        VkImage generated=te_output_image(r->temporal,(unsigned)intermediate-1u);
        VkImageMemoryBarrier ready=gfg_test_barrier(generated,VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(r->output_cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ready);
        VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={width,height,1}};
        vkCmdCopyImage(r->output_cmd,generated,VK_IMAGE_LAYOUT_GENERAL,r->output_images[out_idx],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    } else
#endif
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
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
        intermediate?"LMC":"ORIGINAL",
#else
        intermediate?"AB_SPLIT":"ORIGINAL",
#endif
        (unsigned long long)r->output_presents,
        (unsigned long long)r->output_originals,
        (unsigned long long)r->output_intermediates);
    return VK_SUCCESS;
}

#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
/* Actual GPU LMC, not A|B diagnostic pixels. This isolated prototype uses the
 * source-pool retirement worker as ONE output owner. All CPU fence waits are
 * in this worker; application Present only queues capture and returns. */
static inline VkResult gfg_test_lmc_start(gfg_test_loader_runtime *r,VkInstance instance,
        PFN_vkGetInstanceProcAddr gipa,PFN_vkGetDeviceProcAddr gdpa,
        PFN_vkSetDeviceLoaderData loader_data) {
    /* Experimental post-owner proof only: one temporal analysis per A/B,
     * with 1/2/3 distinct GPU outputs at immutable subframe positions.
     * Reject two competing mode overrides instead of silently choosing. */
    if(getenv("GFG_NATIVE_TEST_LMC_X3") && getenv("GFG_NATIVE_TEST_LMC_X4"))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    r->temporal_multiplier=getenv("GFG_NATIVE_TEST_LMC_X4")?4u:
                           getenv("GFG_NATIVE_TEST_LMC_X3")?3u:2u;
    r->temporal_outputs=r->temporal_multiplier-1u;
    for(unsigned i=0;i<r->temporal_outputs;++i) {
        r->temporal_tau[i]=(float)(i+1u)/(float)r->temporal_multiplier;
        r->temporal_slot_ids[i]=i;
    }
    if(getenv("GFG_NATIVE_TEST_LMC_MASK2")) {
        if(r->temporal_multiplier!=3)return VK_ERROR_FEATURE_NOT_PRESENT;
        r->temporal_outputs=1;r->temporal_tau[0]=0.5f;r->temporal_slot_ids[0]=1;
    }
    VkResult result=te_device_borrow(instance,gipa,r->proxy->physical,r->device,gdpa,
        loader_data,r->queue,r->family,&r->temporal_device);
    if(result!=VK_SUCCESS)return result;
    te_context_desc desc;te_context_desc_default(&desc,r->proxy->source.extent.width,
        r->proxy->source.extent.height,r->temporal_outputs);
    desc.host_io=0;desc.source_format=VK_FORMAT_UNDEFINED;
    desc.source_bgra=r->proxy->source.format==VK_FORMAT_B8G8R8A8_UNORM || r->proxy->source.format==VK_FORMAT_B8G8R8A8_SRGB;
    desc.output_bgra=desc.source_bgra;
    desc.params.algorithm=TE_ALGO_LMC_GPU;desc.params.cut_thr=2;
    result=te_context_create(r->temporal_device,&desc,&r->temporal);
    if(result!=VK_SUCCESS){fprintf(stderr,"LMC owner context: %s\n",te_last_error());return result;}
    /* TEST ONLY: retain two GPU-copied source snapshots and independently
     * read the real LMC destination. Never compiled into a normal payload. */
    r->verify_image_bytes=(VkDeviceSize)desc.width*desc.height*4;
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=3*r->verify_image_bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    result=vkCreateBuffer(r->device,&bc,NULL,&r->verify_buffer);if(result!=VK_SUCCESS)return result;
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(r->device,r->verify_buffer,&req);
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties=(PFN_vkGetPhysicalDeviceMemoryProperties)
        gipa(instance,"vkGetPhysicalDeviceMemoryProperties");
    if(!memory_properties)return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDeviceMemoryProperties mp;memory_properties(r->proxy->physical,&mp);
    unsigned mt=0;
    for(;mt<mp.memoryTypeCount;++mt)if((req.memoryTypeBits&(1u<<mt)) &&
        (mp.memoryTypes[mt].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;
    if(mt==mp.memoryTypeCount)return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=mt};
    result=vkAllocateMemory(r->device,&ma,NULL,&r->verify_memory);if(result!=VK_SUCCESS)return result;
    result=vkBindBufferMemory(r->device,r->verify_buffer,r->verify_memory,0);if(result!=VK_SUCCESS)return result;
    result=vkMapMemory(r->device,r->verify_memory,0,VK_WHOLE_SIZE,0,(void**)&r->verify_map);if(result!=VK_SUCCESS)return result;
    gfg_output_pacer_init(&r->output_pacer,90);
    const char *age=getenv("GFG_NATIVE_TEST_POST_MAX_AGE_MS");
    if(age) {
        char *end=NULL;
        const unsigned long ms=strtoul(age,&end,10);
        if(!end||end==age||*end||ms==0 || ms>500u)
            return VK_ERROR_INITIALIZATION_FAILED;
        r->post_max_age_ns=(uint64_t)ms*1000000ull;
    }
    return VK_SUCCESS;
}
static inline VkResult gfg_test_lmc_init(gfg_test_loader_runtime *r,VkInstance instance,
        PFN_vkGetInstanceProcAddr gipa,PFN_vkGetDeviceProcAddr gdpa,
        PFN_vkSetDeviceLoaderData loader_data) {
    /* CreateSwapchain can precede the app's GetDeviceQueue. Defer TE creation
     * until the first capture, after the loader initialized queue dispatch. */
    r->temporal_instance=instance;r->temporal_gipa=gipa;r->temporal_gdpa=gdpa;
    r->temporal_loader=loader_data;return VK_SUCCESS;
}
static inline VkResult gfg_test_lmc_feed(gfg_test_loader_runtime *r,uint32_t image) {
    if(!r->temporal) {
        VkResult init=gfg_test_lmc_start(r,r->temporal_instance,r->temporal_gipa,
            r->temporal_gdpa,r->temporal_loader);
        if(init!=VK_SUCCESS)return init;
    }
    unsigned allowed=r->have_previous?r->temporal_outputs:0u;
    if(allowed && r->post_max_age_ns) {
        const uint64_t now=gfg_pacer_monotonic_ns();
        const gfg_post_deadline_budget budget={
            .refresh_tick_ns=1000000000ull/90u,
            .max_real_age_ns=r->post_max_age_ns,
            .real_present_cost_ns=1000000ull,
            .synthetic_cost_ns=1000000000ull/90u};
        const unsigned admitted=gfg_post_admit_companions(
            &budget,r->captured_ns[image],now,0,allowed);
        /* A reduced multiplier re-distributes tau across the remaining
         * synthetic outputs. Never try to resuscitate an expired pair. */
        allowed=admitted;
        if(allowed<r->temporal_outputs)++r->post_skipped_pairs;
    }
    r->temporal_active_outputs=allowed;
    for(unsigned i=0;i<allowed;i++) {
        r->temporal_tau[i]=(float)(i+1u)/(float)(allowed+1u);
        r->temporal_slot_ids[i]=i;
    }
    VkResult result=vkWaitForFences(r->device,1,&r->output_fence,VK_TRUE,UINT64_MAX);
    if(result!=VK_SUCCESS)return result;
    result=vkResetCommandBuffer(r->output_cmd,0);if(result!=VK_SUCCESS)return result;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    result=vkBeginCommandBuffer(r->output_cmd,&begin);if(result!=VK_SUCCESS)return result;
    VkImage input=te_source_image(r->temporal,r->temporal_slot);
    VkImageMemoryBarrier ready=gfg_test_barrier(input,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(r->output_cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&ready);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .extent={r->proxy->source.extent.width,r->proxy->source.extent.height,1}};
    vkCmdCopyImage(r->output_cmd,r->history.images[image],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        input,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    VkBufferImageCopy read={.bufferOffset=r->temporal_slot*r->verify_image_bytes,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent=copy.extent};
    vkCmdCopyImageToBuffer(r->output_cmd,r->history.images[image],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        r->verify_buffer,1,&read);
    ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(r->output_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&ready);
    result=vkEndCommandBuffer(r->output_cmd);if(result!=VK_SUCCESS)return result;
    result=vkResetFences(r->device,1,&r->output_fence);if(result!=VK_SUCCESS)return result;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&r->output_cmd};
    result=vkQueueSubmit(r->queue,1,&submit,r->output_fence);if(result!=VK_SUCCESS)return result;
    te_submit work={.slot=r->temporal_slot,.count=r->temporal_active_outputs,
        .timestamps={r->temporal_tau[0],r->temporal_tau[1],r->temporal_tau[2]},
        .nonblocking=1,.lmc_tier=2};
    result=te_submit_frame(r->temporal,&work);
    if(result==VK_NOT_READY) {
        /* Prototype only: source pool bounds outstanding images. This is a
         * worker wait, never the game's Present; not a production latency claim. */
        result=te_wait_idle(r->temporal);if(result==VK_SUCCESS)result=te_submit_frame(r->temporal,&work);
    }
    if(result==VK_SUCCESS)r->temporal_slot^=1u;
    return result;
}
static inline VkResult gfg_test_lmc_verify(gfg_test_loader_runtime *r,unsigned output,uint64_t source,uint32_t image) {
    VkResult result=vkWaitForFences(r->device,1,&r->output_fence,VK_TRUE,UINT64_MAX);
    if(result!=VK_SUCCESS)return result;
    result=vkResetCommandBuffer(r->output_cmd,0);if(result!=VK_SUCCESS)return result;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    result=vkBeginCommandBuffer(r->output_cmd,&begin);if(result!=VK_SUCCESS)return result;
    const int duplicate=getenv("GFG_NATIVE_TEST_LMC_VERIFY_DUPLICATE")!=NULL;
    VkImage generated=duplicate?r->history.images[image]:te_output_image(r->temporal,output);
    VkImageLayout layout=duplicate?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;
    VkImageMemoryBarrier ready=gfg_test_barrier(generated,layout,layout,
        duplicate?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(r->output_cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&ready);
    VkBufferImageCopy read={.bufferOffset=2*r->verify_image_bytes,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={r->proxy->source.extent.width,r->proxy->source.extent.height,1}};
    vkCmdCopyImageToBuffer(r->output_cmd,generated,layout,r->verify_buffer,1,&read);
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(r->output_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,NULL,0,NULL);
    result=vkEndCommandBuffer(r->output_cmd);if(result!=VK_SUCCESS)return result;
    result=vkResetFences(r->device,1,&r->output_fence);if(result!=VK_SUCCESS)return result;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&r->output_cmd};
    result=vkQueueSubmit(r->queue,1,&submit,r->output_fence);if(result!=VK_SUCCESS)return result;
    result=vkWaitForFences(r->device,1,&r->output_fence,VK_TRUE,UINT64_MAX);if(result!=VK_SUCCESS)return result;
    const double tau=r->temporal_tau[output];
    const unsigned char *a=r->verify_map+r->temporal_slot*r->verify_image_bytes;
    const unsigned char *b=r->verify_map+(r->temporal_slot^1u)*r->verify_image_bytes;
    const unsigned char *g=r->verify_map+2*r->verify_image_bytes;
    unsigned distinct_a=0,distinct_b=0,max_error=0;
    for(VkDeviceSize px=0;px<r->verify_image_bytes;px+=4)for(unsigned c=0;c<3;++c) {
        const int expected=(int)((double)a[px+c]*(1-tau)+(double)b[px+c]*tau+0.5);
        int error=abs((int)g[px+c]-expected);
        if((unsigned)error>max_error)max_error=(unsigned)error;
        distinct_a|=abs((int)g[px+c]-(int)a[px+c])>2;
        distinct_b|=abs((int)g[px+c]-(int)b[px+c])>2;
    }
    if(max_error>2 || !distinct_a || !distinct_b) {
        fprintf(stderr,"GSF_LMC_GPU_VERIFY FAIL source=%llu output=%u A=%u B=%u G=%u max_error=%u distinct=%u/%u\n",
            (unsigned long long)source,output,a[0],b[0],g[0],max_error,distinct_a,distinct_b);
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    ++r->verify_outputs;
    fprintf(stderr,"GSF_LMC_GPU_VERIFY PASS source_A=%llu source_B=%llu output=%u tau=%.9f slot_id=%u pixels=%llu max_error=%u distinct_from_A=1 distinct_from_B=1 physical_scanout=UNVERIFIED\n",
        (unsigned long long)(source-1),(unsigned long long)source,output,tau,r->temporal_slot_ids[output],
        (unsigned long long)(r->verify_image_bytes/4),max_error);
    return VK_SUCCESS;
}
#endif

/* GPU-copy retirement worker owns output presentation. Once B arrives,
 * output A/B marked composite BEFORE original B. Initial A is unmodified.
 * The pause is deliberately test-only pacing to make X11 observation
 * deterministic; real Deck pacing must use compositor feedback. */
static inline VkResult gfg_test_live_output(void *user,
        uint32_t image,uint64_t ticket) {
    gfg_test_loader_runtime *r=user;
    if(!r||!r->started||!ticket||image!=ticket%r->history.count)
        return VK_ERROR_INITIALIZATION_FAILED;
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
    if(r->temporal_instance){VkResult feed=gfg_test_lmc_feed(r,image);if(feed!=VK_SUCCESS)return feed;}
#endif
    if(r->interleave && r->have_previous) {
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
        const unsigned outputs=r->temporal?r->temporal_active_outputs:1u;
#else
        const unsigned outputs=1;
#endif
        for(unsigned out=1;out<=outputs;++out) {
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
            if(r->temporal){VkResult verified=gfg_test_lmc_verify(r,out-1,ticket,image);if(verified!=VK_SUCCESS)return verified;}
#endif
            VkResult result=gfg_test_live_emit(r,image,r->last_history_image,(int)out);
            if(result!=VK_SUCCESS)return result;
        }
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
#ifdef GFG_NATIVE_TEST_LMC_OUTPUT_OWNER
    if(r->post_max_age_ns)
        fprintf(stderr,"GSF_POST_OWNER_ADMISSION max_real_age_ms=%.1f skipped_pairs=%llu originals=%llu intermediates=%llu real_preserved=1\n",
            (double)r->post_max_age_ns/1000000.0,
            (unsigned long long)r->post_skipped_pairs,
            (unsigned long long)r->output_originals,
            (unsigned long long)r->output_intermediates);
    if(r->verify_map)vkUnmapMemory(r->device,r->verify_memory);
    if(r->verify_buffer)vkDestroyBuffer(r->device,r->verify_buffer,NULL);
    if(r->verify_memory)vkFreeMemory(r->device,r->verify_memory,NULL);
    if(r->temporal)te_context_destroy(r->temporal);
    if(r->temporal_device)te_device_destroy(r->temporal_device);
#endif
    if(r->output_fence)vkDestroyFence(r->device,r->output_fence,NULL);
    if(r->output_pool)vkDestroyCommandPool(r->device,r->output_pool,NULL);
    for(uint32_t i=0;i<GFG_OFFSCREEN_MAX_IMAGES;i++)
        if(r->ready[i])vkDestroySemaphore(r->device,r->ready[i],NULL);
    if(r->acquired)vkDestroySemaphore(r->device,r->acquired,NULL);
    gfg_offscreen_pool_destroy(&r->history,r->device,&r->memory_ops);
    r->device=VK_NULL_HANDLE;r->started=0;
}
#endif

#define _POSIX_C_SOURCE 200809L
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFG Native CPU transport for the TE8 LMC generator.
 * This deliberately favours correctness and isolation over performance.
 * Copy real presentable swapchain image -> mapped HOST_VISIBLE staging,
 * feed RGB to OWN Temporal Engine, read generated RGB, upload it to an
 * independently acquired swapchain image, present synthetic BEFORE real.
 * No MAKO, no proprietary shaders, no Lossless.dll, no external-FD import.
 *
 * The transport is opt-in, synchronous, SDR8-only, single-swapchain/queue.
 * Inability to initialize returns NULL and leaves normal WSI untouched.
 * Once we have consumed the application's binary render semaphores the
 * fallback is a forward present with waitSemaphoreCount=0 only AFTER the
 * source copy fence signals, so application frame delivery stays correct.
 */
#include "cpu_bridge.h"
#include "te_engine.h"
#include "memory_policy.h"
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOAD(d, name) ((PFN_vk##name)(d)->gdpa((d)->device,"vk" #name))
#define FN(name) PFN_vk##name name
#define COLOR_BPP 4u

/* Fail closed across repeated swapchain lifetimes within one game process.
 * Do not keep paying a half-second first-frame stall at every recreation. */
static atomic_int process_cpu_bridge_slow = ATOMIC_VAR_INIT(0);

struct gfg_cpu_fg {
    VkDevice device;
    VkPhysicalDevice physical;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    uint32_t family;
    VkExtent2D extent;
    VkFormat format;
    PFN_vkGetDeviceProcAddr gdpa;
    FN(CreateBuffer);
    FN(GetBufferMemoryRequirements);
    FN(AllocateMemory);
    FN(BindBufferMemory);
    FN(MapMemory);
    FN(UnmapMemory);
    FN(FreeMemory);
    FN(DestroyBuffer);
    FN(FlushMappedMemoryRanges);
    FN(InvalidateMappedMemoryRanges);
    FN(CreateCommandPool);
    FN(DestroyCommandPool);
    FN(AllocateCommandBuffers);
    FN(ResetCommandBuffer);
    FN(BeginCommandBuffer);
    FN(EndCommandBuffer);
    FN(CmdPipelineBarrier);
    FN(CmdCopyImageToBuffer);
    FN(CmdCopyBufferToImage);
    FN(CreateSemaphore);
    FN(DestroySemaphore);
    FN(CreateFence);
    FN(DestroyFence);
    FN(ResetFences);
    FN(WaitForFences);
    FN(QueueSubmit);
    FN(QueueWaitIdle);
    FN(AcquireNextImageKHR);
    FN(GetSwapchainImagesKHR);
    VkImage *images;
    uint32_t image_count;
    VkBuffer stage;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize allocation_bytes;
    VkDeviceSize atom_bytes;
    int coherent;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkSemaphore acquire_sem;
    VkSemaphore *ready_per_image; /* WSI ownership: one present wait per image */
    unsigned max_frame_ms;
    uint64_t bypasses;
    te_device *engine;
    te_context *context;
    uint8_t *rgba_cached; /* CPU-cached scratch, never byte-sample mapped VRAM */
    double readback_copy_ms;
    double readback_swizzle_ms;
    double upload_pack_ms;
    double upload_copy_ms;
    int host_cached;
    uint32_t memory_index;
    VkMemoryPropertyFlags memory_flags;
    uint8_t *rgb_real;
    uint8_t *rgb_generated;
    uint8_t *rgb_previous;
    uint64_t distinct_frames;
    uint32_t source_slot;
    uint64_t real_frames;
    uint64_t synthetic_frames;
    uint64_t errors;
    uint64_t skipped;
    int permanently_disabled;
    bool log;
};

static void fail(const char *event, VkResult value) {
    fprintf(stderr,"[gfg-native] %s vkresult=%d\n",event,value);
}
static bool ok(VkResult r) { return r==VK_SUCCESS; }
static double elapsed_ms_since(struct timespec start) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return 1000.0 * (double)(now.tv_sec - start.tv_sec)
         + (double)(now.tv_nsec - start.tv_nsec) / 1000000.0;
}
static void disable_slow_bridge(gfg_cpu_fg *fg, const char *stage,
                                double duration_ms) {
    if (fg->permanently_disabled) return;
    fg->permanently_disabled = 1;
    atomic_store_explicit(&process_cpu_bridge_slow, 1, memory_order_relaxed);
    ++fg->bypasses;
    fprintf(stderr,
        "[gfg-native] watchdog disabled generator stage=%s elapsed_ms=%.2f"
        " budget_ms=%u pid=%ld: forwarding real frames only; process breaker active\n",
        stage, duration_ms, fg->max_frame_ms, (long)getpid());
}
static bool coherent_flush(gfg_cpu_fg *fg, VkBool32 invalidate) {
    if(fg->coherent) return true;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=fg->memory,.offset=0,.size=VK_WHOLE_SIZE};
    const VkResult result=invalidate
        ? fg->InvalidateMappedMemoryRanges(fg->device,1,&range)
        : fg->FlushMappedMemoryRanges(fg->device,1,&range);
    return ok(result);
}
static bool record_image_copy(gfg_cpu_fg *fg, VkImage image,
        uint32_t direction) {
    /* direction 0 = image -> staging; direction 1 = staging -> image */
    if(!ok(fg->ResetCommandBuffer(fg->command,0))) return false;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(!ok(fg->BeginCommandBuffer(fg->command,&begin))) return false;
    VkImageMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=0,
        .dstAccessMask=direction?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=direction?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout=direction?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    fg->CmdPipelineBarrier(fg->command,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&before);
    VkBufferImageCopy copy={.bufferOffset=0,.bufferRowLength=0,.bufferImageHeight=0,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={fg->extent.width,fg->extent.height,1}};
    if(direction)
        fg->CmdCopyBufferToImage(fg->command,fg->stage,image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    else
        fg->CmdCopyImageToBuffer(fg->command,image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fg->stage,1,&copy);
    VkImageMemoryBarrier after=before;
    after.srcAccessMask=direction?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    after.dstAccessMask=0;
    after.oldLayout=before.newLayout;
    after.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    fg->CmdPipelineBarrier(fg->command,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&after);
    return ok(fg->EndCommandBuffer(fg->command));
}
static bool submit_wait(gfg_cpu_fg *fg, uint32_t sem_count,
        const VkSemaphore *sems, VkSemaphore signal) {
    VkPipelineStageFlags *stages=NULL;
    if(sem_count) {
        stages=calloc(sem_count,sizeof(*stages));
        if(!stages) return false;
        for(uint32_t i=0;i<sem_count;i++)stages[i]=VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount=sem_count,.pWaitSemaphores=sems,
        .pWaitDstStageMask=stages,
        .commandBufferCount=1,.pCommandBuffers=&fg->command,
        .signalSemaphoreCount=signal?1u:0u,.pSignalSemaphores=signal?&signal:NULL};
    VkResult result=fg->ResetFences(fg->device,1,&fg->fence);
    if(result==VK_SUCCESS)
        result=fg->QueueSubmit(fg->queue,1,&si,fg->fence);
    free(stages);
    if(!ok(result)){fail("queue-submit",result);return false;}
    /* Do NOT replay the game's consumed semaphore on a fallback path.
     * The captured copy must finish before readback or forwarding the real
     * present without those binary waits. */
    result=fg->WaitForFences(fg->device,1,&fg->fence,VK_TRUE,UINT64_MAX);
    if(!ok(result)){fail("queue-wait",result);return false;}
    return true;
}
static int is_bgra_format(VkFormat fmt) {
    return fmt == VK_FORMAT_B8G8R8A8_UNORM ||
           fmt == VK_FORMAT_B8G8R8A8_SRGB;
}
static int supported_sdr8_format(VkFormat fmt) {
    return fmt == VK_FORMAT_R8G8B8A8_UNORM ||
           fmt == VK_FORMAT_B8G8R8A8_UNORM ||
           fmt == VK_FORMAT_R8G8B8A8_SRGB ||
           fmt == VK_FORMAT_B8G8R8A8_SRGB;
}
static void rgba_to_rgb(gfg_cpu_fg *fg) {
    const size_t pixels=(size_t)fg->extent.width*fg->extent.height;
    struct timespec started={0};
    clock_gettime(CLOCK_MONOTONIC,&started);
    /* A bulk memcpy allows native optimized wider reads. Do not perform
     * three million uncached MMIO/BAR byte reads on the Deck APU. */
    memcpy(fg->rgba_cached,fg->mapped,pixels*COLOR_BPP);
    fg->readback_copy_ms=elapsed_ms_since(started);
    const uint8_t *p=fg->rgba_cached;
    uint8_t *out=fg->rgb_real;
    if(is_bgra_format(fg->format)) {
        for(size_t k=0;k<pixels;k++) {
            out[3*k+0]=p[4*k+2];
            out[3*k+1]=p[4*k+1];
            out[3*k+2]=p[4*k+0];
        }
    } else {
        for(size_t k=0;k<pixels;k++) {
            out[3*k+0]=p[4*k+0];
            out[3*k+1]=p[4*k+1];
            out[3*k+2]=p[4*k+2];
        }
    }
    fg->readback_swizzle_ms=elapsed_ms_since(started)-fg->readback_copy_ms;
}
static void rgb_to_rgba(gfg_cpu_fg *fg) {
    const size_t pixels=(size_t)fg->extent.width*fg->extent.height;
    struct timespec started={0};
    clock_gettime(CLOCK_MONOTONIC,&started);
    uint8_t *p=fg->rgba_cached;
    const uint8_t *in=fg->rgb_generated;
    if(is_bgra_format(fg->format)) {
        for(size_t k=0;k<pixels;k++) {
            p[4*k+0]=in[3*k+2];
            p[4*k+1]=in[3*k+1];
            p[4*k+2]=in[3*k+0];
            p[4*k+3]=255;
        }
    } else {
        for(size_t k=0;k<pixels;k++) {
            p[4*k+0]=in[3*k+0];
            p[4*k+1]=in[3*k+1];
            p[4*k+2]=in[3*k+2];
            p[4*k+3]=255;
        }
    }
    fg->upload_pack_ms=elapsed_ms_since(started);
    /* Host-to-device upload remains a bulk write, not 4m byte stores. */
    memcpy(fg->mapped,fg->rgba_cached,pixels*COLOR_BPP);
    fg->upload_copy_ms=elapsed_ms_since(started)-fg->upload_pack_ms;
}
static bool run_te(gfg_cpu_fg *fg) {
    uint32_t slot=fg->source_slot;
    struct timespec start={0};
    clock_gettime(CLOCK_MONOTONIC,&start);
    const VkResult upload_result=te_upload_rgb(fg->context,slot,fg->rgb_real);
    const double upload_ms=elapsed_ms_since(start);
    if(upload_result!=VK_SUCCESS) {
        fprintf(stderr,"[gfg-native] TE upload: %s\n",te_last_error());
        return false;
    }
    te_submit s={0};
    s.slot=slot;
    s.count=fg->real_frames?1:0;
    s.timestamps[0]=0.5f;
    s.download=1;
    const char *tier=getenv("GFG_NATIVE_LMC_TIER");
    if(tier && tier[0]>='0' && tier[0]<='2' && tier[1]=='\0')
        s.lmc_tier=tier[0]-'0';
    const VkResult v=te_submit_frame(fg->context,&s);
    const double submitted_ms=elapsed_ms_since(start);
    if(v!=VK_SUCCESS) { fail("te-submit",v); return false; }
    uint64_t budget=250000000ULL;
    const char *budget_env=getenv("GFG_NATIVE_BUDGET_MS");
    if(budget_env && *budget_env) {
        char *end=NULL;
        const unsigned long raw=strtoul(budget_env,&end,10);
        if(end && !*end && raw>=10 && raw<=5000)
            budget=(uint64_t)raw*1000000ULL;
    }
    const int ready=te_context_idle(fg->context,budget);
    const double finished_wait_ms=elapsed_ms_since(start);
    if(fg->log) fprintf(stderr,
        "[gfg-native] TE pair slot=%u frame=%llu tier=%d ready=%d waited_ms=%.1f\n",
        slot,(unsigned long long)fg->real_frames,s.lmc_tier,ready,
        finished_wait_ms-submitted_ms);
    double download_ms=0.0;
    if(ready && fg->real_frames) {
        const VkResult downloaded=te_download_rgb(fg->context,0,fg->rgb_generated);
        download_ms=elapsed_ms_since(start)-finished_wait_ms;
        if(downloaded!=VK_SUCCESS) {
            fprintf(stderr,"[gfg-native] TE download: %s\n",te_last_error());
            return false;
        }
    }
    if(fg->log && (fg->real_frames<5 || fg->real_frames%30==0 ||
                   elapsed_ms_since(start)>30.0))
        fprintf(stderr,
            "[gfg-native] TE stages frame=%llu upload_ms=%.2f"
            " submit_ms=%.2f wait_ms=%.2f download_ms=%.2f"
            " total_ms=%.2f ready=%d pid=%ld\n",
            (unsigned long long)fg->real_frames,upload_ms,
            submitted_ms-upload_ms,finished_wait_ms-submitted_ms,
            download_ms,elapsed_ms_since(start),ready,(long)getpid());
    if(!ready) ++fg->skipped;
    return ready;
}
static bool present_generated(gfg_cpu_fg *fg, PFN_vkQueuePresentKHR next) {
    uint32_t idx=0;
    VkResult r=fg->AcquireNextImageKHR(fg->device,fg->swapchain,0,
        fg->acquire_sem,VK_NULL_HANDLE,&idx);
    if((r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR)||idx>=fg->image_count) {
        ++fg->skipped;
        return false;
    }
    const VkSemaphore present_ready = fg->ready_per_image[idx];
    /* Khronos swapchain semaphore reuse: associate the present-wait semaphore
     * with the acquired image, not with a CPU frame. Our queue submission
     * waits on acquire_sem, and its fence ensures that wait completed before
     * present_ready can be signaled again for the same image.
     * Do not wait for the entire WSI queue after presenting a synthetic. */
    rgb_to_rgba(fg);
    if(fg->log && (fg->synthetic_frames<5 || fg->upload_copy_ms>15.0))
        fprintf(stderr,
            "[gfg-native] CPU output pack_ms=%.2f memcpy_to_gpu_ms=%.2f"
            " cached=%d\n",
            fg->upload_pack_ms,fg->upload_copy_ms,fg->host_cached);
    if(!coherent_flush(fg,VK_FALSE) ||
       !record_image_copy(fg,fg->images[idx],1) ||
       !submit_wait(fg,1,&fg->acquire_sem,present_ready)) {
        ++fg->errors;
        fg->permanently_disabled=1;
        return false;
    }
    VkPresentInfoKHR output={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount=1,.pWaitSemaphores=&present_ready,
        .swapchainCount=1,.pSwapchains=&fg->swapchain,.pImageIndices=&idx};
    r=next(fg->queue,&output);
    if(r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR) {
        ++fg->errors;
        fg->permanently_disabled=1;
        return false;
    }
    fg->synthetic_frames++;
    if(fg->log && fg->synthetic_frames%30==1)
        fprintf(stderr,"[gfg-native] generated=%llu real=%llu skipped=%llu errors=%llu\n",
            (unsigned long long)fg->synthetic_frames,
            (unsigned long long)fg->real_frames,
            (unsigned long long)fg->skipped,
            (unsigned long long)fg->errors);
    return true;
}

gfg_cpu_fg *gfg_cpu_fg_create(VkInstance instance,
        PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice physical,
        VkDevice device, PFN_vkGetDeviceProcAddr gdpa,
        VkSwapchainKHR swapchain, VkExtent2D extent, VkFormat format,
        VkQueue queue, uint32_t family) {
    if(!physical || !device || !queue || !instance || !gipa || !gdpa ||
       !extent.width || !extent.height || extent.width>3840 || extent.height>2160 ||
       !supported_sdr8_format(format))
        return NULL;
    if(atomic_load_explicit(&process_cpu_bridge_slow,memory_order_relaxed)) {
        if(getenv("GFG_NATIVE_TRACE"))
            fprintf(stderr,
                "[gfg-native] process breaker: skip CPU bridge on new swapchain pid=%ld\n",
                (long)getpid());
        return NULL;
    }
    gfg_cpu_fg *fg=calloc(1,sizeof(*fg));
    if(!fg)return NULL;
    fg->device=device;fg->physical=physical;fg->queue=queue;
    fg->gdpa=gdpa;fg->swapchain=swapchain;fg->extent=extent;fg->format=format;
    fg->family=family;fg->log=getenv("GFG_NATIVE_TRACE")!=NULL;
    fg->max_frame_ms=100; /* watchdog guards pathological WSI stalls */
    const char *guard=getenv("GFG_NATIVE_MAX_FRAME_MS");
    if(guard && *guard) {
        char *end=NULL;
        unsigned long v=strtoul(guard,&end,10);
        if(end && *end=='\0' && v>=20 && v<=10000)
            fg->max_frame_ms=(unsigned)v;
    }
#define RESOLVE(name) fg->name=LOAD(fg,name); if(!fg->name) goto fail
    RESOLVE(CreateBuffer);
    RESOLVE(GetBufferMemoryRequirements);
    RESOLVE(AllocateMemory);
    RESOLVE(BindBufferMemory);
    RESOLVE(MapMemory);
    RESOLVE(UnmapMemory);
    RESOLVE(FreeMemory);
    RESOLVE(DestroyBuffer);
    RESOLVE(FlushMappedMemoryRanges);
    RESOLVE(InvalidateMappedMemoryRanges);
    RESOLVE(CreateCommandPool);
    RESOLVE(DestroyCommandPool);
    RESOLVE(AllocateCommandBuffers);
    RESOLVE(ResetCommandBuffer);
    RESOLVE(BeginCommandBuffer);
    RESOLVE(EndCommandBuffer);
    RESOLVE(CmdPipelineBarrier);
    RESOLVE(CmdCopyImageToBuffer);
    RESOLVE(CmdCopyBufferToImage);
    RESOLVE(CreateSemaphore);
    RESOLVE(DestroySemaphore);
    RESOLVE(CreateFence);
    RESOLVE(DestroyFence);
    RESOLVE(ResetFences);
    RESOLVE(WaitForFences);
    RESOLVE(QueueSubmit);
    RESOLVE(QueueWaitIdle);
    RESOLVE(AcquireNextImageKHR);
    RESOLVE(GetSwapchainImagesKHR);
#undef RESOLVE
    PFN_vkGetPhysicalDeviceMemoryProperties mp=(PFN_vkGetPhysicalDeviceMemoryProperties)
        gipa(instance,"vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceProperties2 props2=(PFN_vkGetPhysicalDeviceProperties2)
        gipa(instance,"vkGetPhysicalDeviceProperties2");
    if(!mp || !props2)goto fail;
    uint32_t n=0;
    if(fg->GetSwapchainImagesKHR(device,swapchain,&n,NULL)!=VK_SUCCESS||
       n<2||n>32)goto fail;
    fg->images=calloc(n,sizeof(*fg->images));
    if(!fg->images||fg->GetSwapchainImagesKHR(device,swapchain,&n,fg->images)!=VK_SUCCESS)
        goto fail;
    fg->image_count=n;
    fg->ready_per_image=calloc(n,sizeof(*fg->ready_per_image));
    if(!fg->ready_per_image)goto fail;
    const size_t pixels=(size_t)extent.width*extent.height;
    if(pixels>SIZE_MAX/4u)goto fail;
    const VkDeviceSize bytes=(VkDeviceSize)pixels*COLOR_BPP;
    fg->rgba_cached=malloc(pixels*COLOR_BPP);
    fg->rgb_real=malloc(pixels*3u);
    fg->rgb_generated=malloc(pixels*3u);
    fg->rgb_previous=malloc(pixels*3u);
    if(!fg->rgba_cached||!fg->rgb_real||!fg->rgb_generated||!fg->rgb_previous)
        goto fail;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(!ok(fg->CreateBuffer(device,&bi,NULL,&fg->stage)))goto fail;
    VkMemoryRequirements mr;
    fg->GetBufferMemoryRequirements(device,fg->stage,&mr);
    VkPhysicalDeviceMemoryProperties types;
    mp(physical,&types);
    uint32_t index=0;
    if(!gfg_native_readback_memory_type(&types,mr.memoryTypeBits,&index,
                                       &fg->coherent,&fg->host_cached))
        goto fail;
    fg->memory_index=index;
    fg->memory_flags=types.memoryTypes[index].propertyFlags;
    if(fg->log)
        fprintf(stderr,
            "[gfg-native] staging memory type=%u flags=0x%x"
            " host_cached=%d coherent=%d allocation_bytes=%llu\n",
            index,(unsigned)fg->memory_flags,fg->host_cached,fg->coherent,
            (unsigned long long)mr.size);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=mr.size,.memoryTypeIndex=index};
    if(!ok(fg->AllocateMemory(device,&ai,NULL,&fg->memory)))goto fail;
    fg->allocation_bytes=mr.size;
    if(!ok(fg->BindBufferMemory(device,fg->stage,fg->memory,0)))goto fail;
    if(!ok(fg->MapMemory(device,fg->memory,0,VK_WHOLE_SIZE,0,&fg->mapped)))goto fail;
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    if(!ok(fg->CreateCommandPool(device,&pool,NULL,&fg->pool)))goto fail;
    VkCommandBufferAllocateInfo cmd={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=fg->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount=1};
    if(!ok(fg->AllocateCommandBuffers(device,&cmd,&fg->command)))goto fail;
    VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if(!ok(fg->CreateFence(device,&fci,NULL,&fg->fence)))goto fail;
    VkSemaphoreCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if(!ok(fg->CreateSemaphore(device,&sci,NULL,&fg->acquire_sem)))goto fail;
    for(uint32_t i=0;i<fg->image_count;i++)
        if(!ok(fg->CreateSemaphore(device,&sci,NULL,&fg->ready_per_image[i])))
            goto fail;

    struct timespec te_start={0};
    clock_gettime(CLOCK_MONOTONIC,&te_start);
    VkPhysicalDeviceIDProperties id={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext=&id};
    props2(physical,&properties);
    te_device_desc dd={.match_uuid=1};
    memcpy(dd.device_uuid,id.deviceUUID,VK_UUID_SIZE);
    if(te_device_create(&dd,&fg->engine)!=VK_SUCCESS){
        fprintf(stderr,"[gfg-native] TE device: %s\n",te_last_error());goto fail;
    }
    te_context_desc cd;
    te_context_desc_default(&cd,extent.width,extent.height,1);
    const char *algorithm=getenv("GFG_NATIVE_ALGO");
    cd.params.algorithm=(!algorithm || strcmp(algorithm,"lmc")) ?
        TE_ALGO_TILES : TE_ALGO_LMC;
    cd.host_io=1;
    if(te_context_create(fg->engine,&cd,&fg->context)!=VK_SUCCESS){
        fprintf(stderr,"[gfg-native] TE context: %s\n",te_last_error());goto fail;
    }
    if(fg->log)fprintf(stderr,
        "[gfg-native] OWN TE8 CPU bridge active: %ux%u format=%d"
        " algo=%s startup_ms=%.2f pid=%ld\n",
        extent.width,extent.height,format,
        cd.params.algorithm==TE_ALGO_LMC?"lmc":"tiles",
        elapsed_ms_since(te_start),(long)getpid());
    return fg;
fail:
    gfg_cpu_fg_destroy(fg);
    return NULL;
}
VkResult gfg_cpu_fg_present(gfg_cpu_fg *fg, const VkPresentInfoKHR *original,
                            PFN_vkQueuePresentKHR next) {
    if(!fg || !original || !next)return VK_ERROR_INITIALIZATION_FAILED;
    if(fg->permanently_disabled)return next(fg->queue,original);
    struct timespec start={0};
    clock_gettime(CLOCK_MONOTONIC,&start);
    const uint32_t index=original->pImageIndices[0];
    if(index>=fg->image_count)return next(fg->queue,original);
    if(!record_image_copy(fg,fg->images[index],0))
        return next(fg->queue,original); /* app semaphore untouched */
    if(!submit_wait(fg,original->waitSemaphoreCount,
            original->pWaitSemaphores,VK_NULL_HANDLE)) {
        /* A failing submit MAY have consumed waits. Never replay an unknown
         * binary semaphore. Device-loss failure propagates, not a fake success. */
        fg->permanently_disabled=1;
        return VK_ERROR_DEVICE_LOST;
    }
    VkPresentInfoKHR real=*original;
    real.waitSemaphoreCount=0;
    real.pWaitSemaphores=NULL;
    const double capture_ms=elapsed_ms_since(start);
    if(capture_ms > (double)fg->max_frame_ms)
        disable_slow_bridge(fg,"capture-fence",capture_ms);
    if(!fg->permanently_disabled && !coherent_flush(fg,VK_TRUE))
        fg->permanently_disabled=1;
    if(!fg->permanently_disabled) {
        const double convert_start_ms=elapsed_ms_since(start);
        rgba_to_rgb(fg);
        const double converted_ms=elapsed_ms_since(start);
        if(fg->log && fg->real_frames<5)
            fprintf(stderr,
                "[gfg-native] CPU convert frame=%llu invalidate_ms=%.2f"
                " rgba_to_rgb_ms=%.2f memcpy_from_gpu_ms=%.2f"
                " swizzle_ms=%.2f host_cached=%d\n",
                (unsigned long long)fg->real_frames,
                convert_start_ms-capture_ms,converted_ms-convert_start_ms,
                fg->readback_copy_ms,fg->readback_swizzle_ms,fg->host_cached);
        const bool completed=run_te(fg);
        if(completed && fg->real_frames) {
            const size_t count=(size_t)fg->extent.width*fg->extent.height*3u;
            uint64_t delta_old=0,delta_new=0;
            for(size_t i=0;i<count;i++){
                const int a=fg->rgb_generated[i];
                const int b=fg->rgb_previous[i];
                const int c=fg->rgb_real[i];
                delta_old+=(uint64_t)abs(a-b);
                delta_new+=(uint64_t)abs(a-c);
            }
            const int distinct=delta_old>0 && delta_new>0;
            if(fg->log)fprintf(stderr,
                "[gfg-native] candidate distinct=%d delta_old=%llu delta_new=%llu\n",
                distinct,(unsigned long long)delta_old,
                (unsigned long long)delta_new);
            const double before_present_ms=elapsed_ms_since(start);
            if(before_present_ms > (double)fg->max_frame_ms) {
                disable_slow_bridge(fg,"capture-plus-te8",before_present_ms);
            } else if (distinct) {
                if(present_generated(fg,next)) ++fg->distinct_frames;
            } else {
                ++fg->skipped; /* never queue a duplicate synthetic frame */
            }
        }
        if(!completed && fg->context && !te_context_idle(fg->context,0)) {
            /* Avoid overwriting source slot while old compute work persists.
             * New pairs resume only after a context reset in a later version.
             * Real frames remain unaffected after this disable. */
            fg->permanently_disabled=1;
        }
        memcpy(fg->rgb_previous,fg->rgb_real,
            (size_t)fg->extent.width*fg->extent.height*3u);
        fg->source_slot ^= 1u;
    }
    fg->real_frames++;
    const double before_real_ms=elapsed_ms_since(start);
    if(before_real_ms > (double)fg->max_frame_ms)
        disable_slow_bridge(fg,"before-real-present",before_real_ms);
    const VkResult presented=next(fg->queue,&real);
    const double total_ms=elapsed_ms_since(start);
    if(total_ms > (double)fg->max_frame_ms)
        disable_slow_bridge(fg,"real-present",total_ms);
    if(fg->log && (total_ms > 30.0 || fg->real_frames%60==0))
        fprintf(stderr,
            "[gfg-native] timing capture_ms=%.2f pre_real_ms=%.2f"
            " total_ms=%.2f generated=%llu bypassed=%d\n",
            capture_ms,before_real_ms,total_ms,
            (unsigned long long)fg->synthetic_frames,
            fg->permanently_disabled);
    return presented;
}
void gfg_cpu_fg_destroy(gfg_cpu_fg *fg) {
    if(!fg)return;
    if(fg->log)fprintf(stderr,
        "[gfg-native] CPU bridge retired real=%llu generated=%llu distinct=%llu missed=%llu errors=%llu bypasses=%llu\n",
        (unsigned long long)fg->real_frames,
        (unsigned long long)fg->synthetic_frames,
        (unsigned long long)fg->distinct_frames,
        (unsigned long long)fg->skipped,
        (unsigned long long)fg->errors,
        (unsigned long long)fg->bypasses);
    if(fg->context)te_context_destroy(fg->context);
    if(fg->engine)te_device_destroy(fg->engine);
    if(fg->device && fg->QueueWaitIdle)(void)fg->QueueWaitIdle(fg->queue);
    if(fg->device && fg->DestroySemaphore) {
        if(fg->ready_per_image)for(uint32_t i=0;i<fg->image_count;i++)
            if(fg->ready_per_image[i])
                fg->DestroySemaphore(fg->device,fg->ready_per_image[i],NULL);
        if(fg->acquire_sem)fg->DestroySemaphore(fg->device,fg->acquire_sem,NULL);
    }
    if(fg->device && fg->DestroyFence && fg->fence)
        fg->DestroyFence(fg->device,fg->fence,NULL);
    if(fg->device && fg->DestroyCommandPool && fg->pool)
        fg->DestroyCommandPool(fg->device,fg->pool,NULL);
    if(fg->device && fg->UnmapMemory && fg->mapped)
        fg->UnmapMemory(fg->device,fg->memory);
    if(fg->device && fg->DestroyBuffer && fg->stage)
        fg->DestroyBuffer(fg->device,fg->stage,NULL);
    if(fg->device && fg->FreeMemory && fg->memory)
        fg->FreeMemory(fg->device,fg->memory,NULL);
    free(fg->ready_per_image);
    free(fg->images);
    free(fg->rgba_cached);
    free(fg->rgb_real);
    free(fg->rgb_generated);
    free(fg->rgb_previous);
    free(fg);
}
uint64_t gfg_cpu_fg_generated(const gfg_cpu_fg *fg) {
    return fg?fg->synthetic_frames:0;
}

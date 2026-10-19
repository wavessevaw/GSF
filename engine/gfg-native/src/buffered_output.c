#define _GNU_SOURCE
/* SPDX-License-Identifier: GPL-3.0-or-later
 * Two independent workers: capture retirement releases game images after
 * snapshot completion; output retains snapshots until all generation and
 * transport GPU reads finish. Only the capture submission uses the game
 * Present thread. No synthetic/readiness/WSI wait occurs on that thread.
 */
#include "buffered_output.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#define CAP GFG_VIRTUAL_MAX_IMAGES
#define API(X) \
 X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
 X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdPipelineBarrier) X(CmdCopyImage) X(CreateFence) X(DestroyFence) \
 X(ResetFences) X(WaitForFences) X(CreateSemaphore) X(DestroySemaphore) \
 X(QueueSubmit) X(QueueWaitIdle) X(AcquireNextImageKHR) X(GetSwapchainImagesKHR)
struct gsf_buffered_output {
    gfg_gpu_fg_create_info info;
    gfg_output_proxy proxy;
    gfg_offscreen_source_pool snapshots;
    gfg_offscreen_dispatch memory;
    PFN_vkDestroySwapchainKHR destroy;
    PFN_vkQueuePresentKHR present;
#define DECL(n) PFN_vk##n n;
    API(DECL)
#undef DECL
    gfg_gpu_fg *gpu;
    FILE *telemetry;
    VkCommandPool capture_pool,output_pool;
    VkCommandBuffer capture[CAP],output;
    VkFence captured[CAP],output_done;
    VkSemaphore acquired,ready[64];
    VkImage images[64];
    uint32_t image_count;
    uint8_t seen[64],snapshot_seen[CAP],used[CAP];
    struct {uint64_t token,ns;unsigned snapshot;} jobs[CAP];
    struct {uint64_t token,ns;unsigned snapshot;} output_jobs[CAP];
    unsigned capture_ids[CAP],capture_head,capture_count,output_head,output_count;
    uint64_t serial,captured_count,dropped,output_count_total;
    uint64_t last_capture_ns,native_period_ns;
    pthread_mutex_t lock,wsi;
    pthread_cond_t changed;
    pthread_t retire_worker,output_worker;
    int initialized,retire_started,output_started,capture_busy,output_busy;
    atomic_int stopping;
    atomic_int failure;
};
static uint64_t tick(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
/* Called under owner lock. Buffer timings are observations/estimates, never
 * fabricated device timestamps or display latency measurements. */
static void buffer_event(gsf_buffered_output *o,const char *event,uint64_t token,uint64_t capture_ns) {
    if(!o->telemetry)return;
    unsigned occupancy=0;for(unsigned i=0;i<CAP;++i)occupancy+=o->used[i]!=0;
    uint64_t now=tick();
    fprintf(o->telemetry,"{\"schema\":2,\"event\":\"buffer_owner\",\"operation\":\"%s\","
        "\"source_id\":%llu,\"observed_ns\":%llu,\"capture_ns\":%llu,\"buffer_age_ns\":%llu,"
        "\"occupancy\":%u,\"capacity\":%u,\"capturing\":%u,\"queued\":%u,\"dropped_real\":%llu,"
        "\"captured_real\":%llu,\"output_real\":%llu,\"displayed\":null}\n",
        event,(unsigned long long)token,(unsigned long long)now,(unsigned long long)capture_ns,
        (unsigned long long)(capture_ns && now>=capture_ns?now-capture_ns:0),occupancy,CAP,
        o->capture_count,o->output_count,(unsigned long long)o->dropped,
        (unsigned long long)o->captured_count,(unsigned long long)o->output_count_total);
}
static VkImageMemoryBarrier ib(VkImage image,VkImageLayout before,VkImageLayout after,
                              VkAccessFlags src,VkAccessFlags dst) {
    return (VkImageMemoryBarrier){.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image=image,.oldLayout=before,.newLayout=after,.srcAccessMask=src,.dstAccessMask=dst,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
}
static void fail_owner(gsf_buffered_output *o,VkResult r) {
    atomic_store(&o->failure,r);
    gfg_vwsi_close(&o->proxy.app);
    pthread_mutex_lock(&o->lock);o->stopping=1;
    pthread_cond_broadcast(&o->changed);pthread_mutex_unlock(&o->lock);
}
static VkResult signal_acquire(void *user,VkSemaphore sem,VkFence fence) {
    gsf_buffered_output *o=user;
    VkResult r=atomic_load(&o->failure);if(r!=VK_SUCCESS)return r;
    VkSubmitInfo s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount=sem?1u:0u,.pSignalSemaphores=&sem};
    return o->QueueSubmit(o->info.queue,1,&s,fence);
}
static VkResult capture(void *user,uint32_t source,uint64_t token,
                        uint32_t wait_count,const VkSemaphore *waits) {
    gsf_buffered_output *o=user;
    if(source>=o->proxy.source.count || wait_count>16)return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&o->lock);
    VkResult r=atomic_load(&o->failure);
    if(o->stopping || r!=VK_SUCCESS) {pthread_mutex_unlock(&o->lock);return r<0?r:VK_ERROR_OUT_OF_DATE_KHR;}
    unsigned slot=0;while(slot<CAP && o->used[slot])++slot;
    /* Reclaim only an obsolete GPU-ready job which the output worker has
     * not acquired. Never overwrite an executing/capturing snapshot. */
    if(slot==CAP && o->output_count) {
        slot=o->output_jobs[o->output_head].snapshot;
        o->output_head=(o->output_head+1)%CAP;--o->output_count;++o->dropped;
    }
    if(slot<CAP)o->used[slot]=1;
    VkCommandBuffer cmd=o->capture[source];
    r=o->ResetCommandBuffer(cmd,0);if(r!=VK_SUCCESS)goto done;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    r=o->BeginCommandBuffer(cmd,&begin);if(r!=VK_SUCCESS)goto done;
    if(slot<CAP) {
        VkImageMemoryBarrier b=ib(o->snapshots.images[slot],o->snapshot_seen[slot]?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        o->CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
        VkImageCopy c={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={o->info.extent.width,o->info.extent.height,1}};
        o->CmdCopyImage(cmd,o->proxy.source.images[source],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            o->snapshots.images[slot],VK_IMAGE_LAYOUT_GENERAL,1,&c);
        b=ib(o->snapshots.images[slot],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        o->CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    } else ++o->dropped; // bounded overload: consume waits and retire source, never repeat as synthetic
    r=o->EndCommandBuffer(cmd);if(r!=VK_SUCCESS)goto done;
    r=o->ResetFences(o->info.device,1,&o->captured[source]);if(r!=VK_SUCCESS)goto done;
    VkPipelineStageFlags stages[16];for(unsigned i=0;i<wait_count;++i)stages[i]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=wait_count,.pWaitSemaphores=waits,
        .pWaitDstStageMask=stages,.commandBufferCount=1,.pCommandBuffers=&cmd};
    r=o->QueueSubmit(o->info.queue,1,&s,o->captured[source]);if(r!=VK_SUCCESS)goto done;
    const uint64_t captured_ns=tick();
    if(o->last_capture_ns && captured_ns>o->last_capture_ns)o->native_period_ns=captured_ns-o->last_capture_ns;
    o->last_capture_ns=captured_ns;
    o->jobs[source].token=token;o->jobs[source].ns=captured_ns;o->jobs[source].snapshot=slot;
    if(slot<CAP)o->snapshot_seen[slot]=1;
    o->capture_ids[(o->capture_head+o->capture_count)%CAP]=source;++o->capture_count;++o->serial;
    buffer_event(o,"capture_submitted",token,o->jobs[source].ns);
    pthread_cond_broadcast(&o->changed);
done:
    pthread_mutex_unlock(&o->lock);
    if(r<0)fail_owner(o,r);
    return r;
}
static void *retire_main(void *user) {
    gsf_buffered_output *o=user;
    for(;;) {
        pthread_mutex_lock(&o->lock);
        while(!o->capture_count && !o->stopping)pthread_cond_wait(&o->changed,&o->lock);
        if(!o->capture_count){pthread_mutex_unlock(&o->lock);break;}
        unsigned source=o->capture_ids[o->capture_head];
        o->capture_head=(o->capture_head+1)%CAP;--o->capture_count;o->capture_busy=1;
        uint64_t token=o->jobs[source].token,ns=o->jobs[source].ns;unsigned slot=o->jobs[source].snapshot;
        pthread_mutex_unlock(&o->lock);
        VkResult r=o->WaitForFences(o->info.device,1,&o->captured[source],VK_TRUE,UINT64_MAX);
        if(r!=VK_SUCCESS){fail_owner(o,r);break;}
        /* Snapshot owns its image independently of the released app index. */
        r=gfg_vwsi_retire(&o->proxy.app,source,token,1);
        if(r!=VK_SUCCESS && !o->stopping){fail_owner(o,r);break;}
        pthread_mutex_lock(&o->lock);if(slot<CAP)++o->captured_count;o->capture_busy=0;
        if(slot<CAP) {
            unsigned at=(o->output_head+o->output_count)%CAP;
            o->output_jobs[at].token=token;o->output_jobs[at].ns=ns;o->output_jobs[at].snapshot=slot;
            ++o->output_count;
        }
        buffer_event(o,slot<CAP?"capture_ready":"capture_dropped",token,ns);
        pthread_cond_broadcast(&o->changed);pthread_mutex_unlock(&o->lock);
    }
    return NULL;
}
static VkResult emit(gsf_buffered_output *o,unsigned snapshot,uint64_t token,uint64_t captured_ns,uint64_t native_period) {
    VkResult r=o->WaitForFences(o->info.device,1,&o->output_done,VK_TRUE,UINT64_MAX);
    if(r!=VK_SUCCESS)return r;
    unsigned idx;
    r=o->AcquireNextImageKHR(o->info.device,o->proxy.output,1000000000ull,o->acquired,VK_NULL_HANDLE,&idx);
    if(r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR)return r;
    if(idx>=o->image_count)return VK_ERROR_OUT_OF_DATE_KHR;
    r=o->ResetCommandBuffer(o->output,0);if(r!=VK_SUCCESS)return r;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    r=o->BeginCommandBuffer(o->output,&begin);if(r!=VK_SUCCESS)return r;
    VkImageMemoryBarrier b=ib(o->images[idx],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    o->CmdPipelineBarrier(o->output,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    VkImageCopy c={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .extent={o->info.extent.width,o->info.extent.height,1}};
    o->CmdCopyImage(o->output,o->snapshots.images[snapshot],VK_IMAGE_LAYOUT_GENERAL,o->images[idx],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
    b=ib(o->images[idx],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0);
    o->CmdPipelineBarrier(o->output,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&b);
    r=o->EndCommandBuffer(o->output);if(r!=VK_SUCCESS)return r;
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo s={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&o->acquired,
        .pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&o->output,
        .signalSemaphoreCount=1,.pSignalSemaphores=&o->ready[idx]};
    r=o->QueueSubmit(o->info.queue,1,&s,VK_NULL_HANDLE);if(r!=VK_SUCCESS)return r;
    VkPresentInfoKHR p={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&o->ready[idx],
        .swapchainCount=1,.pSwapchains=&o->proxy.output,.pImageIndices=&idx};
    gfg_gpu_fg_set_native_period(o->gpu,native_period);
    gfg_gpu_fg_set_source(o->gpu,captured_ns,tick(),token);
    gfg_gpu_fg_set_output_pacing(o->gpu,1);
    r=gfg_gpu_fg_present(o->gpu,o->info.queue,&p,o->present,NULL);
    gfg_gpu_fg_set_output_pacing(o->gpu,0);
    /* Fence covers bridge analysis, every synthetic copy and real transport.
     * Snapshot cannot be recycled based on capture completion alone. */
    if(r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR)return r;
    r=o->ResetFences(o->info.device,1,&o->output_done);if(r!=VK_SUCCESS)return r;
    r=o->QueueSubmit(o->info.queue,0,NULL,o->output_done);if(r!=VK_SUCCESS)return r;
    r=o->WaitForFences(o->info.device,1,&o->output_done,VK_TRUE,UINT64_MAX);

    return r;
}
static void *output_main(void *user) {
    gsf_buffered_output *o=user;
    for(;;) {
        pthread_mutex_lock(&o->lock);
        while(!o->output_count && !o->stopping)pthread_cond_wait(&o->changed,&o->lock);
        if(!o->output_count){pthread_mutex_unlock(&o->lock);break;}
        unsigned at=o->output_head,slot=o->output_jobs[at].snapshot;
        uint64_t token=o->output_jobs[at].token,ns=o->output_jobs[at].ns,period=o->native_period_ns;
        o->output_head=(at+1)%CAP;--o->output_count;o->output_busy=1;
        pthread_mutex_unlock(&o->lock);
        VkResult r=emit(o,slot,token,ns,period);
        if(r!=VK_SUCCESS){fail_owner(o,r);break;}
        pthread_mutex_lock(&o->lock);o->used[slot]=0;o->output_busy=0;++o->output_count_total;
        buffer_event(o,"real_output_retired",token,ns);
        pthread_cond_broadcast(&o->changed);pthread_mutex_unlock(&o->lock);
    }
    return NULL;
}
gfg_output_proxy *gsf_buffered_proxy(gsf_buffered_output *o){return o?&o->proxy:NULL;}
int gsf_buffered_uses_queue(const gsf_buffered_output *o,VkQueue q){return o && o->info.queue==q;}
VkResult gsf_buffered_status(gsf_buffered_output *o){return o?atomic_load(&o->failure):VK_ERROR_INITIALIZATION_FAILED;}
VkResult gsf_buffered_drain(gsf_buffered_output *o) {
    if(!o || !o->initialized)return VK_SUCCESS;
    pthread_mutex_lock(&o->lock);
    while((o->capture_count || o->capture_busy || o->output_count || o->output_busy) && !o->stopping)
        pthread_cond_wait(&o->changed,&o->lock);
    VkResult r=atomic_load(&o->failure);
    pthread_mutex_unlock(&o->lock);return r;
}
void gsf_buffered_stop(gsf_buffered_output *o) {
    if(!o)return;
    if(o->initialized) {
        pthread_mutex_lock(&o->lock);o->stopping=1;pthread_cond_broadcast(&o->changed);pthread_mutex_unlock(&o->lock);
        if(o->retire_started)pthread_join(o->retire_worker,NULL);
        if(o->output_started)pthread_join(o->output_worker,NULL);
    }
    o->retire_started=o->output_started=0;
    if(o->proxy.app_initialized)gfg_vwsi_close(&o->proxy.app);
}
void gsf_buffered_destroy(gsf_buffered_output *o) {
    if(!o)return;
    gsf_buffered_stop(o);
    if(o->gpu)gfg_gpu_fg_destroy(o->gpu);
    if(o->QueueWaitIdle)o->QueueWaitIdle(o->info.queue);
    for(unsigned i=0;i<CAP;++i)if(o->captured[i])o->DestroyFence(o->info.device,o->captured[i],NULL);
    if(o->output_done)o->DestroyFence(o->info.device,o->output_done,NULL);
    if(o->capture_pool)o->DestroyCommandPool(o->info.device,o->capture_pool,NULL);
    if(o->output_pool)o->DestroyCommandPool(o->info.device,o->output_pool,NULL);
    if(o->acquired)o->DestroySemaphore(o->info.device,o->acquired,NULL);
    for(unsigned i=0;i<64;++i)if(o->ready[i])o->DestroySemaphore(o->info.device,o->ready[i],NULL);
    gfg_offscreen_pool_destroy(&o->snapshots,o->info.device,&o->memory);
    gfg_output_proxy_destroy(&o->proxy,o->destroy,&o->memory);
    if(o->initialized){pthread_cond_destroy(&o->changed);pthread_mutex_destroy(&o->lock);pthread_mutex_destroy(&o->wsi);}
    if(o->telemetry)fclose(o->telemetry);
    free(o);
}
gsf_buffered_output *gsf_buffered_create(const gfg_gpu_fg_create_info *info,
    const VkSwapchainCreateInfoKHR *source,const VkSwapchainCreateInfoKHR *real,
    PFN_vkCreateSwapchainKHR create,PFN_vkDestroySwapchainKHR destroy,
    PFN_vkQueuePresentKHR present,const gfg_offscreen_dispatch *memory) {
    gsf_buffered_output *o=calloc(1,sizeof(*o));if(!o)return NULL;
    o->info=*info;o->memory=*memory;o->destroy=destroy;o->present=present;
#define LOAD(n) o->n=(PFN_vk##n)info->gdpa(info->device,"vk" #n);if(!o->n)goto fail;
    API(LOAD)
#undef LOAD
    if(pthread_mutex_init(&o->lock,NULL))goto fail;
    if(pthread_mutex_init(&o->wsi,NULL)){pthread_mutex_destroy(&o->lock);goto fail;}
    if(pthread_cond_init(&o->changed,NULL)){pthread_mutex_destroy(&o->lock);pthread_mutex_destroy(&o->wsi);goto fail;}
    o->initialized=1;
    const char *trace=getenv("GFG_NATIVE_TELEMETRY");
    if(trace && *trace) {
        char path[4096];int size=snprintf(path,sizeof(path),"%s.buffer.jsonl",trace);
        if(size>0 && size<(int)sizeof(path)) {
            int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
            if(fd>=0){o->telemetry=fdopen(fd,"w");if(!o->telemetry)close(fd);}
        }
    }
    if(gfg_output_proxy_create_ex(&o->proxy,info->device,info->physical,source,real,create,destroy,memory,o,signal_acquire,capture)!=VK_SUCCESS)goto fail;
    VkSwapchainCreateInfoKHR snapshots=*source;snapshots.minImageCount=CAP;snapshots.imageUsage|=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(gfg_offscreen_pool_create(&o->snapshots,info->device,info->physical,&snapshots,memory)!=VK_SUCCESS)goto fail;
    VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=info->family};
    if(o->CreateCommandPool(info->device,&pc,NULL,&o->capture_pool)!=VK_SUCCESS ||
       o->CreateCommandPool(info->device,&pc,NULL,&o->output_pool)!=VK_SUCCESS)goto fail;
    VkCommandBufferAllocateInfo ac={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=o->capture_pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=o->proxy.source.count};
    if(o->AllocateCommandBuffers(info->device,&ac,o->capture)!=VK_SUCCESS)goto fail;
    ac.commandPool=o->output_pool;ac.commandBufferCount=1;
    if(o->AllocateCommandBuffers(info->device,&ac,&o->output)!=VK_SUCCESS)goto fail;
    if(info->set_loader_data) {
        for(unsigned i=0;i<o->proxy.source.count;++i)if(info->set_loader_data(info->device,o->capture[i])!=VK_SUCCESS)goto fail;
        if(info->set_loader_data(info->device,o->output)!=VK_SUCCESS)goto fail;
    }
    VkFenceCreateInfo f={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for(unsigned i=0;i<o->proxy.source.count;++i)if(o->CreateFence(info->device,&f,NULL,&o->captured[i])!=VK_SUCCESS)goto fail;
    f.flags=VK_FENCE_CREATE_SIGNALED_BIT;if(o->CreateFence(info->device,&f,NULL,&o->output_done)!=VK_SUCCESS)goto fail;
    o->image_count=64;
    if(o->GetSwapchainImagesKHR(info->device,o->proxy.output,&o->image_count,o->images)!=VK_SUCCESS || !o->image_count)goto fail;
    VkSemaphoreCreateInfo sem={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if(o->CreateSemaphore(info->device,&sem,NULL,&o->acquired)!=VK_SUCCESS)goto fail;
    for(unsigned i=0;i<o->image_count;++i)if(o->CreateSemaphore(info->device,&sem,NULL,&o->ready[i])!=VK_SUCCESS)goto fail;
    o->info.swapchain=o->proxy.output;
    o->gpu=gfg_gpu_fg_create(&o->info);if(!o->gpu)goto fail;
    gfg_gpu_fg_set_output_thread(o->gpu,&o->wsi);
    if(pthread_create(&o->retire_worker,NULL,retire_main,o))goto fail;
    o->retire_started=1;
    if(pthread_create(&o->output_worker,NULL,output_main,o))goto fail;
    o->output_started=1;
    fprintf(stderr,"[gsf-mi] buffered-owner active source_images=%u snapshot_capacity=%u output_images=%u distinct_source_output=1\n",o->proxy.source.count,CAP,o->image_count);
    return o;
fail:
    gsf_buffered_destroy(o);return NULL;
}

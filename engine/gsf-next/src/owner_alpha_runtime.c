/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "owner_alpha_runtime.h"
#include "layer.h"
#include <string.h>

static void cleanup(struct gsf_owner_alpha_runtime *r)
{
    struct afmf_device *dev=r->device;
    if(!dev)return;
    /* Only after all submitted GPU work is externally retired. */
    for(uint32_t i=0;i<r->slot_count;i++) {
        if(r->ingest_done[i])
            dev->fns.destroy_fence(dev->handle,r->ingest_done[i],NULL);
        if(r->reconstruct_done[i])
            dev->fns.destroy_fence(dev->handle,r->reconstruct_done[i],NULL);
    }
    if(r->commands)
        dev->fns.destroy_command_pool(dev->handle,r->commands,NULL);
    if(r->record_lock_ready) {
        pthread_mutex_destroy(&r->record_lock);
        r->record_lock_ready=false;
    }
    if(r->generator)
        afmf_framegen_destroy(dev,r->generator);
    memset(r,0,sizeof(*r));
}

VkResult gsf_owner_alpha_runtime_init(
    struct gsf_owner_alpha_runtime *r,
    struct afmf_device *dev,VkQueue queue,pthread_mutex_t *queue_mutex,
    uint32_t family,VkFormat format,VkExtent2D extent,
    const VkImage *snapshots,uint32_t snapshot_count,
    const VkImage *targets,uint32_t target_count,uint32_t slots,
    bool direct_ingest,const struct afmf_config *config,
    const struct gsf_alpha_owner_callbacks *callbacks,void *user)
{
    if(!r||!dev||!queue||!queue_mutex||!snapshots||!targets||!config||
       !callbacks||!config->interpolate||!dev->set_loader_data||
       !dev->fns.create_command_pool||!dev->fns.destroy_command_pool||
       !dev->fns.allocate_command_buffers||!dev->fns.create_fence||
       !dev->fns.destroy_fence||!dev->fns.wait_for_fences||
       !dev->fns.reset_fences||!dev->fns.queue_submit||
       !slots||slots>GSF_ALPHA_OWNER_MAX_SLOTS||
       !snapshot_count||snapshot_count>GSF_OWNER_SOURCES||
       (config->multiplier!=2u && config->multiplier!=3u)||
       target_count!=slots*(config->multiplier-1u)||
       !extent.width||!extent.height||!dev->queue_families||
       family>=dev->queue_family_count||
       (dev->queue_families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)==0)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* Immutable one-to-one mapping: descriptor indices are stable for
     * the whole context lifetime. Aliased snapshots or duplicate target
     * lanes would race across the asynchronous GPU work slots. */
    for(uint32_t i=0;i<snapshot_count;i++) {
        if(!snapshots[i])return VK_ERROR_INITIALIZATION_FAILED;
        for(uint32_t j=0;j<i;j++)
            if(snapshots[i]==snapshots[j])return VK_ERROR_INITIALIZATION_FAILED;
    }
    for(uint32_t i=0;i<target_count;i++) {
        if(!targets[i])return VK_ERROR_INITIALIZATION_FAILED;
        for(uint32_t j=0;j<snapshot_count;j++)
            if(targets[i]==snapshots[j])return VK_ERROR_INITIALIZATION_FAILED;
        for(uint32_t j=0;j<i;j++)
            if(targets[i]==targets[j])return VK_ERROR_INITIALIZATION_FAILED;
    }
    memset(r,0,sizeof(*r));
    r->device=dev;r->queue=queue;r->queue_mutex=queue_mutex;
    r->slot_count=slots;r->snapshot_count=snapshot_count;
    r->target_count=target_count;
    r->outputs_per_pair=config->multiplier-1u;
    if(pthread_mutex_init(&r->record_lock,NULL)!=0) {
        cleanup(r);return VK_ERROR_INITIALIZATION_FAILED;
    }
    r->record_lock_ready=true;
    memcpy(r->snapshots,snapshots,snapshot_count*sizeof(VkImage));
    memcpy(r->targets,targets,target_count*sizeof(VkImage));
    /* The actual Alpha generator creates FidelityFX flow history, descriptors
     * and reconstructor pipelines against the owner's private snapshots. */
    struct afmf_config compact_config=*config;
    compact_config.owner_target_stride=r->outputs_per_pair;
    r->generator=afmf_framegen_create(dev,format,extent,slots,snapshots,
         snapshot_count,targets,target_count,direct_ingest,&compact_config);
    if(!r->generator) {
        cleanup(r);return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(!gsf_alpha_owner_transport_init(&r->transport,dev,r->generator,
                                      queue,slots,callbacks,user)) {
        cleanup(r);return VK_ERROR_INITIALIZATION_FAILED;
    }
    r->transport.submit_lock=queue_mutex;
    const VkCommandPoolCreateInfo pool={
        .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex=family};
    VkResult result=dev->fns.create_command_pool(dev->handle,&pool,NULL,&r->commands);
    if(result!=VK_SUCCESS){cleanup(r);return result;}
    for(uint32_t i=0;i<slots;i++) {
        VkCommandBuffer buffers[2]={VK_NULL_HANDLE,VK_NULL_HANDLE};
        const VkCommandBufferAllocateInfo alloc={
            .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=r->commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount=2};
        result=dev->fns.allocate_command_buffers(dev->handle,&alloc,buffers);
        if(result!=VK_SUCCESS){cleanup(r);return result;}
        for(uint32_t j=0;j<2;j++) {
            result=dev->set_loader_data(dev->handle,buffers[j]);
            if(result!=VK_SUCCESS){cleanup(r);return result;}
        }
        r->ingest_cmd[i]=buffers[0];r->reconstruct_cmd[i]=buffers[1];
        const VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result=dev->fns.create_fence(dev->handle,&fence,NULL,&r->ingest_done[i]);
        if(result==VK_SUCCESS)
            result=dev->fns.create_fence(dev->handle,&fence,NULL,&r->reconstruct_done[i]);
        if(result!=VK_SUCCESS){cleanup(r);return result;}
    }
    afmf_framegen_set_family(dev,r->generator,family);
    r->started=true;
    return VK_SUCCESS;
}

VkResult gsf_owner_alpha_runtime_submit(
    struct gsf_owner_alpha_runtime *r,uint64_t frame_id,uint64_t deadline_ns,
    uint32_t slot,uint32_t snapshot_index,uint32_t count)
{
    if(!r||!r->started||slot>=r->slot_count||
       snapshot_index>=r->snapshot_count||count>r->outputs_per_pair)
        return VK_ERROR_INITIALIZATION_FAILED;
    struct gsf_owner_alpha_pair work={
        .snapshot=r->snapshots[snapshot_index],.slot=slot,
        .source_index=snapshot_index,.count=count,
        .layout=afmf_framegen_ingest_direct(r->generator)?
                    VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    /* One pinned, unaliased target lane per Alpha work slot. Never write a
     * texture from another in-flight slot just to increase the multiplier. */
    for(uint32_t i=0;i<r->outputs_per_pair;i++) {
        const uint32_t ix=slot*r->outputs_per_pair+i;
        work.targets[i]=r->targets[ix];
        work.indices[i]=ix;
        if(i<count)work.phases[i]=(float)(i+1u)/(float)(count+1u);
    }
    /* Other jobs cannot modify Alpha's CPU frame_index / 2-frame history
     * during recording. Only the two actual vkQueueSubmit calls take the
     * shared game Vulkan-queue mutex, inside the transport/recorder. */
    pthread_mutex_lock(&r->record_lock);
    VkResult result=gsf_alpha_owner_enqueue(&r->transport,frame_id,deadline_ns,
         slot,r->ingest_cmd[slot],r->reconstruct_cmd[slot],
         r->ingest_done[slot],r->reconstruct_done[slot],&work);
    pthread_mutex_unlock(&r->record_lock);
    return result;
}

VkResult gsf_owner_alpha_runtime_poll(struct gsf_owner_alpha_runtime *r,uint64_t now_ns)
{
    return r&&r->started?gsf_alpha_owner_poll(&r->transport,now_ns):
                        VK_ERROR_INITIALIZATION_FAILED;
}

bool gsf_owner_alpha_runtime_output_done(
    struct gsf_owner_alpha_runtime *r,uint32_t slot,uint64_t frame_id,
    VkFence output_copy_done)
{
    return r&&r->started&&gsf_alpha_owner_output_done(
         &r->transport,slot,frame_id,output_copy_done);
}

bool gsf_owner_alpha_runtime_destroy(struct gsf_owner_alpha_runtime *r)
{
    if(!r)return false;
    if(!r->started) {
        if(r->device)cleanup(r);
        return true;
    }
    if(gsf_alpha_owner_live(&r->transport))return false;
    cleanup(r);
    return true;
}

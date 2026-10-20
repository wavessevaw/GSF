/* SPDX-License-Identifier: MIT */
#include "owner_alpha_record.h"
#include "layer.h"
#include <stdbool.h>
#include <stddef.h>

static void transition(const struct afmf_device *dev,VkCommandBuffer cmd,
                       VkImage image,VkImageLayout from,VkImageLayout to,
                       VkAccessFlags before,VkAccessFlags after,
                       VkPipelineStageFlags src,VkPipelineStageFlags dst)
{
    const VkImageMemoryBarrier b={
        .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=before,.dstAccessMask=after,
        .oldLayout=from,.newLayout=to,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    dev->fns.cmd_pipeline_barrier(cmd,src,dst,0,0,NULL,0,NULL,1,&b);
}

VkResult gsf_owner_alpha_record(struct afmf_device *dev,
                               struct afmf_framegen *fg,
                               VkCommandBuffer ingest_cmd,
                               VkCommandBuffer reconstruct_cmd,
                               const struct gsf_owner_alpha_pair *job)
{
    if(!dev||!fg||!job||!job->snapshot||!ingest_cmd||!reconstruct_cmd||
       ingest_cmd==reconstruct_cmd||job->count>GSF_OWNER_ALPHA_OUTPUTS||
       !dev->fns.begin_command_buffer||!dev->fns.end_command_buffer||
       !dev->fns.cmd_pipeline_barrier)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(job->slot>=afmf_framegen_owner_slots(fg) ||
       job->source_index>=afmf_framegen_owner_sources(fg))
        return VK_ERROR_INITIALIZATION_FAILED;
    if(job->layout!=(afmf_framegen_ingest_direct(fg)?
                    VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
        return VK_ERROR_FEATURE_NOT_PRESENT;

    const bool direct=afmf_framegen_direct(fg);
    const bool batch=job->count>1 && afmf_framegen_batched(fg);
    /* Batch x3 has 2 physical targets, not 3 dummy textures. The third
     * descriptor aliases lane #2 but the count=2 shader never writes it. */
    const uint32_t views=job->count;
    const uint32_t stride=afmf_framegen_owner_output_stride(fg);
    if(batch && (stride<2u || stride>GSF_OWNER_ALPHA_OUTPUTS ||
                 job->count>stride))
        return VK_ERROR_FEATURE_NOT_PRESENT; /* cannot steal next work slot */
    float prev=0.0f;
    for(uint32_t i=0;i<views;i++) {
        if(!job->targets[i])
            return VK_ERROR_INITIALIZATION_FAILED;
        if(direct&&job->indices[i]>=afmf_framegen_owner_targets(fg))
            return VK_ERROR_INITIALIZATION_FAILED;
        if(batch&&job->indices[i]!=job->slot*stride+i)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if(i<job->count) {
            const float phase=job->phases[i];
            if(!(phase>prev&&phase<1.0f))return VK_ERROR_INITIALIZATION_FAILED;
            prev=phase;
        }
    }

    afmf_framegen_prepare(dev,fg,job->slot,job->count);
    const VkCommandBufferBeginInfo begin={
        .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkResult res=dev->fns.begin_command_buffer(ingest_cmd,&begin);
    if(res!=VK_SUCCESS)return res;
    /* Snapshot read only. Its layout and contents belong to the owner. */
    afmf_framegen_record_ingest(dev,fg,ingest_cmd,job->slot,
        job->snapshot,job->source_index,job->targets,job->indices,
        job->phases,job->count);
    res=dev->fns.end_command_buffer(ingest_cmd);
    if(res!=VK_SUCCESS)return res;

    res=dev->fns.begin_command_buffer(reconstruct_cmd,&begin);
    if(res!=VK_SUCCESS)return res;
    const VkPipelineStageFlags compute=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkPipelineStageFlags copy=VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags writer=direct?compute:copy;
    const VkAccessFlags write=direct?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkImageLayout write_layout=direct?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    for(uint32_t i=0;i<views;i++)
        transition(dev,reconstruct_cmd,job->targets[i],VK_IMAGE_LAYOUT_UNDEFINED,
                   write_layout,0,write,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,writer);
    /* Actual generator from GSF Next Alpha: FidelityFX Optical Flow +
     * interpolation, same kernels and descriptor logic as Alpha. */
    afmf_framegen_record_reconstruct(dev,fg,reconstruct_cmd,job->slot,
        job->snapshot,job->source_index,job->targets,job->indices,
        job->phases,job->count);
    for(uint32_t i=0;i<job->count;i++)
        transition(dev,reconstruct_cmd,job->targets[i],write_layout,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,write,VK_ACCESS_TRANSFER_READ_BIT,
                   writer|copy,copy);
    afmf_framegen_finish(dev,fg,reconstruct_cmd,job->slot);
    return dev->fns.end_command_buffer(reconstruct_cmd);
}

VkResult gsf_owner_alpha_submit_serialized(
    struct afmf_device *dev,struct afmf_framegen *fg,VkQueue queue,
    VkCommandBuffer ingest_cmd,VkCommandBuffer reconstruct_cmd,
    VkFence ingest_done,VkFence reconstruct_done,
    const struct gsf_owner_alpha_pair *job,
    struct gsf_owner_alpha_submit_status *status,
    pthread_mutex_t *queue_lock)
{
    if(!status)return VK_ERROR_INITIALIZATION_FAILED;
    *status=(struct gsf_owner_alpha_submit_status){0};
    if(!dev||!queue||!ingest_done||!reconstruct_done||
       ingest_done==reconstruct_done||
       !dev->fns.queue_submit||!dev->fns.reset_fences)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result=gsf_owner_alpha_record(dev,fg,ingest_cmd,reconstruct_cmd,job);
    if(result!=VK_SUCCESS)return result;
    const VkSubmitInfo ingest={
        .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=1,.pCommandBuffers=&ingest_cmd};
    result=dev->fns.reset_fences(dev->handle,1,&ingest_done);
    if(result!=VK_SUCCESS)return result;
    /* Reset both idle fences BEFORE the first queue submit: a failed
     * reconstruction fence reset must never leave an in-flight source
     * submission with no valid second-stage fence. */
    const VkSubmitInfo warp={
        .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=1,.pCommandBuffers=&reconstruct_cmd};
    result=dev->fns.reset_fences(dev->handle,1,&reconstruct_done);
    if(result!=VK_SUCCESS)return result;
    /* The application's VkQueue is host-external-synchronized. Hold its
     * shared lock ONLY for the two quick Vulkan submits, never for shader
     * command recording or Alpha descriptor preparation. */
    if(queue_lock)pthread_mutex_lock(queue_lock);
    result=dev->fns.queue_submit(queue,1,&ingest,ingest_done);
    if(result==VK_SUCCESS) {
        status->ingest_submitted=true;
        result=dev->fns.queue_submit(queue,1,&warp,reconstruct_done);
        if(result==VK_SUCCESS)status->reconstruct_submitted=true;
    }
    if(queue_lock)pthread_mutex_unlock(queue_lock);
    return result;
}

VkResult gsf_owner_alpha_submit(struct afmf_device *dev,struct afmf_framegen *fg,
    VkQueue queue,VkCommandBuffer ingest_cmd,VkCommandBuffer reconstruct_cmd,
    VkFence ingest_done,VkFence reconstruct_done,
    const struct gsf_owner_alpha_pair *job,
    struct gsf_owner_alpha_submit_status *status)
{
    return gsf_owner_alpha_submit_serialized(dev,fg,queue,ingest_cmd,
        reconstruct_cmd,ingest_done,reconstruct_done,job,status,NULL);
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain_internal.h"

/* Synthetic storage is independent of acquired WSI images. Only the presenter
 * acquires an output image, immediately before copying a ready synthetic to it.
 * Nine resident images cover three bounded jobs with up to three outputs. */
VkResult afmf_sc_output_init(struct afmf_device *dev,struct afmf_swapchain *sc)
{
    const struct afmf_device_fns *f=&dev->fns;
    sc->output_capacity=sc->image_count<3u ? sc->image_count : 3u;
    VkMemoryRequirements reqs[GSF_NEXT_RING_IMAGES];
    VkDeviceSize offsets[GSF_NEXT_RING_IMAGES],bytes=0;
    uint32_t types=UINT32_MAX,count=sc->output_capacity*GSF_NEXT_MAX_OUTPUTS;
    for(uint32_t k=0;k<count;k++) {
        VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
            .format=sc->format,.extent={sc->extent.width,sc->extent.height,1},.mipLevels=1,.arrayLayers=1,
            .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
        VkResult res=f->create_image(dev->handle,&info,NULL,&sc->output_images[k]);
        if(res!=VK_SUCCESS) return res;
        f->get_image_memory_requirements(dev->handle,sc->output_images[k],&reqs[k]);
        VkDeviceSize alignment=reqs[k].alignment;
        bytes=(bytes+alignment-1)/alignment*alignment;offsets[k]=bytes;bytes+=reqs[k].size;
        types &= reqs[k].memoryTypeBits;
    }
    uint32_t type=UINT32_MAX;
    for(uint32_t k=0;k<dev->memory_properties.memoryTypeCount;k++)
        if((types & (1u<<k)) && (dev->memory_properties.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type=k;break; }
    if(type==UINT32_MAX || getenv("GSF_NEXT_TEST_FAIL_OUTPUT_ALLOC")!=NULL) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo memory={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=bytes,.memoryTypeIndex=type};
    VkResult res=f->allocate_memory(dev->handle,&memory,NULL,&sc->output_memory);
    if(res!=VK_SUCCESS) return res;
    sc->output_bytes=bytes;
    for(uint32_t k=0;k<count;k++) {
        res=f->bind_image_memory(dev->handle,sc->output_images[k],sc->output_memory,offsets[k]);
        if(res!=VK_SUCCESS) return res;
    }
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    res=f->create_fence(dev->handle,&fence,NULL,&sc->copy_fence);
    if(res!=VK_SUCCESS) return res;
    sc->output_copy_gpu_ms=-1;
    if(sc->config.profile && dev->queue_families[dev->async_family].timestampValidBits>0) {
        VkQueryPoolCreateInfo queries={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        (void)f->create_query_pool(dev->handle,&queries,NULL,&sc->copy_queries);
    }
    return VK_SUCCESS;
}

bool afmf_sc_output_pool(struct afmf_device *dev,struct afmf_swapchain *sc,uint32_t family)
{
    if(sc->copy_pool!=VK_NULL_HANDLE) return true;
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    if(dev->fns.create_command_pool(dev->handle,&pool,NULL,&sc->copy_pool)!=VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=sc->copy_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(dev->fns.allocate_command_buffers(dev->handle,&alloc,&sc->copy_cmd)!=VK_SUCCESS) return false;
    return dev->set_loader_data!=NULL && dev->set_loader_data(dev->handle,sc->copy_cmd)==VK_SUCCESS;
}

void afmf_sc_output_destroy(struct afmf_device *dev,struct afmf_swapchain *sc)
{
    if(sc->copy_queries!=VK_NULL_HANDLE) dev->fns.destroy_query_pool(dev->handle,sc->copy_queries,NULL);
    if(sc->copy_pool!=VK_NULL_HANDLE) dev->fns.destroy_command_pool(dev->handle,sc->copy_pool,NULL);
    if(sc->copy_fence!=VK_NULL_HANDLE) dev->fns.destroy_fence(dev->handle,sc->copy_fence,NULL);
    for(uint32_t k=0;k<GSF_NEXT_RING_IMAGES;k++)
        if(sc->output_images[k]!=VK_NULL_HANDLE) dev->fns.destroy_image(dev->handle,sc->output_images[k],NULL);
    if(sc->output_memory!=VK_NULL_HANDLE) dev->fns.free_memory(dev->handle,sc->output_memory,NULL);
    memset(sc->output_images,0,sizeof sc->output_images);
    sc->output_memory=VK_NULL_HANDLE;sc->copy_pool=VK_NULL_HANDLE;
    sc->copy_fence=VK_NULL_HANDLE;sc->copy_queries=VK_NULL_HANDLE;
    sc->copy_cmd=VK_NULL_HANDLE;sc->output_capacity=0; /* Keep peak allocation bytes for the final report. */
}

/* The generation fence has retired and the WSI acquire fence has been observed.
 * Copy command buffers have a separate pool from the concurrent work thread. */
VkResult afmf_sc_output_copy(struct afmf_device *dev,struct afmf_swapchain *sc,
                             const struct afmf_present_job *job,uint32_t output,uint32_t *index)
{
    struct timespec start,end;(void)clock_gettime(CLOCK_MONOTONIC,&start);
    uint64_t budget=2*gsf_refresh_period(job->config.refresh_hz),spent=0;
    while(!afmf_sc_spare_take(dev,sc,index)) {
        (void)clock_gettime(CLOCK_MONOTONIC,&end);
        spent=(uint64_t)(end.tv_sec-start.tv_sec)*1000000000ull+(uint64_t)end.tv_nsec-(uint64_t)start.tv_nsec;
        if(spent>=budget) return VK_NOT_READY;
        struct timespec pause={.tv_nsec=200000};(void)nanosleep(&pause,NULL);
    }
    (void)clock_gettime(CLOCK_MONOTONIC,&end);
    double acquire_ms=elapsed_ms(&start,&end);
    const struct afmf_device_fns *f=&dev->fns;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkResult res=f->begin_command_buffer(sc->copy_cmd,&begin);if(res!=VK_SUCCESS) return res;
    if(sc->copy_queries!=VK_NULL_HANDLE) {
        f->cmd_reset_query_pool(sc->copy_cmd,sc->copy_queries,0,2);
        f->cmd_write_timestamp(sc->copy_cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,sc->copy_queries,0);
    }
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=sc->images[*index],
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    f->cmd_pipeline_barrier(sc->copy_cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkImageCopy region={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .extent={sc->extent.width,sc->extent.height,1}};
    f->cmd_copy_image(sc->copy_cmd,sc->output_images[job->work_slot*GSF_NEXT_MAX_OUTPUTS+output],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        sc->images[*index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    f->cmd_pipeline_barrier(sc->copy_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(sc->copy_queries!=VK_NULL_HANDLE) f->cmd_write_timestamp(sc->copy_cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,sc->copy_queries,1);
    res=f->end_command_buffer(sc->copy_cmd);if(res!=VK_SUCCESS) return res;
    res=f->reset_fences(dev->handle,1,&sc->copy_fence);if(res!=VK_SUCCESS) return res;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&sc->copy_cmd,
        .signalSemaphoreCount=1,.pSignalSemaphores=&sc->sem_generated[*index]};
    pthread_mutex_lock(&dev->async_lock);
    res=f->queue_submit(dev->async_queue,1,&submit,sc->copy_fence);
    pthread_mutex_unlock(&dev->async_lock);
    if(res!=VK_SUCCESS) return res;
    do {res=f->wait_for_fences(dev->handle,1,&sc->copy_fence,VK_TRUE,5000000ull);} while(res==VK_TIMEOUT);
    double gpu_ms=-1;
    if(res==VK_SUCCESS && sc->copy_queries!=VK_NULL_HANDLE) {
        uint64_t ticks[2];
        if(f->get_query_pool_results(dev->handle,sc->copy_queries,0,2,sizeof ticks,ticks,sizeof ticks[0],VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            uint32_t valid=dev->queue_families[dev->async_family].timestampValidBits;
            uint64_t mask=valid>=64 ? UINT64_MAX : (1ull<<valid)-1;
            gpu_ms=(double)((ticks[1]-ticks[0])&mask)*(double)dev->limits.timestampPeriod/1e6;
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC,&end);
    pthread_mutex_lock(&sc->job_lock);
    sc->output_copy_gpu_ms=gpu_ms;
    sc->output_acquire_ms=acquire_ms;sc->output_copy_wait_ms=elapsed_ms(&start,&end)-acquire_ms;
    pthread_mutex_unlock(&sc->job_lock);
    return res;
}

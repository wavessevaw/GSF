/* SPDX-License-Identifier: GPL-3.0-or-later
 * TEST ONLY: independent GPU copies inspect each image before lower WSI
 * presentation. CPU waits/readback here are deliberately outside the runtime.
 * This proves image content and API handoff order, never physical scanout.
 */
#include "gpu_bridge.h"
#include <math.h>
#include <time.h>
#ifdef GFG_NATIVE_TEST_OBSERVE_LAYER
#define OBS_FUNCS(X) \
    X(AllocateCommandBuffers) \
    X(AllocateMemory) \
    X(BeginCommandBuffer) \
    X(BindBufferMemory) \
    X(CmdCopyImageToBuffer) \
    X(CmdPipelineBarrier) \
    X(CreateBuffer) \
    X(CreateCommandPool) \
    X(CreateFence) \
    X(CreateSemaphore) \
    X(DestroyBuffer) \
    X(DestroyCommandPool) \
    X(DestroyFence) \
    X(DestroySemaphore) \
    X(EndCommandBuffer) \
    X(FreeMemory) \
    X(GetBufferMemoryRequirements) \
    X(MapMemory) \
    X(QueueSubmit) \
    X(ResetCommandBuffer) \
    X(ResetFences) \
    X(UnmapMemory) \
    X(WaitForFences)
static struct {
#define OBS_DECLARE(n) PFN_vk##n n;
    OBS_FUNCS(OBS_DECLARE)
#undef OBS_DECLARE
} observer_api;
static PFN_vkSetDeviceLoaderData observer_set_loader_data;
#define OBS(n) observer_api.n
#else
#define OBS(n) vk##n
#endif

static struct {
    VkDevice device;
    VkPhysicalDeviceMemoryProperties memory_props;
    VkQueue queue;
    VkImage *images;
    VkExtent2D extent;
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char *mapped;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkSemaphore *ready;
    uint32_t image_count, frame, interval_calls, total_calls, generated;
    unsigned char previous[4], samples[3][4];
    PFN_vkQueuePresentKHR lower;
    gfg_gpu_fg *fg;
    int failed;
    unsigned multiplier, real_calls, output_mask, steady_pairs;
    uint64_t handoff_ns[16];
} observer;
static VKAPI_ATTR VkResult VKAPI_CALL observer_loader_data(VkDevice device,void *object) {
    (void)device;(void)object;return VK_SUCCESS; /* top-level loader allocated these */
}
static VkResult observer_present(VkQueue queue,const VkPresentInfoKHR *info) {
    struct timespec stamp;clock_gettime(CLOCK_MONOTONIC,&stamp);
    if(observer.total_calls<16)observer.handoff_ns[observer.total_calls]=(uint64_t)stamp.tv_sec*1000000000ull+(uint64_t)stamp.tv_nsec;

    uint32_t index=info->pImageIndices[0];
    if(index>=observer.image_count) return VK_ERROR_INITIALIZATION_FAILED;
    OBS(ResetCommandBuffer)(observer.command,0);
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if(OBS(BeginCommandBuffer)(observer.command,&bi)!=VK_SUCCESS) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageMemoryBarrier before={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=observer.images[index],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    OBS(CmdPipelineBarrier)(observer.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&before);
    VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={observer.extent.width,observer.extent.height,1}};
    OBS(CmdCopyImageToBuffer)(observer.command,observer.images[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        observer.buffer,1,&region);
    VkImageMemoryBarrier after=before;
    after.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;after.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    after.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;after.dstAccessMask=0;
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=observer.buffer,.offset=0,.size=VK_WHOLE_SIZE};
    OBS(CmdPipelineBarrier)(observer.command,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,1,&host,1,&after);
    if(OBS(EndCommandBuffer)(observer.command)!=VK_SUCCESS) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineStageFlags stages[64];
    if(info->waitSemaphoreCount>64) return VK_ERROR_INITIALIZATION_FAILED;
    for(uint32_t i=0;i<info->waitSemaphoreCount;++i) stages[i]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount=info->waitSemaphoreCount,.pWaitSemaphores=info->pWaitSemaphores,.pWaitDstStageMask=stages,
        .commandBufferCount=1,.pCommandBuffers=&observer.command,
        .signalSemaphoreCount=1,.pSignalSemaphores=&observer.ready[index]};
    OBS(ResetFences)(observer.device,1,&observer.fence);
    VkResult r=OBS(QueueSubmit)(queue,1,&submit,observer.fence);
    if(r!=VK_SUCCESS) return r;
    r=OBS(WaitForFences)(observer.device,1,&observer.fence,VK_TRUE,UINT64_MAX);
    if(r!=VK_SUCCESS) return r;
    for(size_t pixel=0;pixel<(size_t)observer.extent.width*observer.extent.height;++pixel)
        for(unsigned c=0;c<3;++c)
            if(abs((int)observer.mapped[4*pixel+c]-(int)observer.mapped[c])>1) observer.failed=1;
    const char *fault=getenv("GFG_NATIVE_GPU_FAULT");
    const int recovery=fault && !strcmp(fault,"record");
    if(recovery){
        if(observer.frame==1 && observer.interval_calls==0)
            memcpy(observer.samples[0],observer.mapped,4);
        else if(observer.frame==1 && observer.interval_calls==1){
            if(memcmp(observer.samples[0],observer.mapped,4)) observer.failed=1;
        }
    } else {
    unsigned m=observer.multiplier?observer.multiplier:3;
    if(observer.interval_calls<m-1 && observer.frame){
        memcpy(observer.samples[observer.interval_calls],observer.mapped,4);
        observer.output_mask|=1u<<observer.interval_calls;++observer.generated;
    }else{
        ++observer.real_calls;
        if(observer.frame){
            for(uint32_t output=0;output<m-1;++output)if(observer.output_mask&(1u<<output)) {
                for(uint32_t c=0;c<3;++c){
                    int expected=((int)(m-1-output)*(int)observer.previous[c]+(int)(1+output)*(int)observer.mapped[c]+(int)m/2)/(int)m;
                    if(abs((int)observer.samples[output][c]-expected)>2) observer.failed=1;
                }
                if(!memcmp(observer.samples[output],observer.previous,3) ||
                   !memcmp(observer.samples[output],observer.mapped,3)) observer.failed=1;
            }
            if(observer.frame>=2 && !getenv("GFG_NATIVE_TEST_CUT") && !getenv("GFG_NATIVE_TEST_DUPLICATE")) {
                if(observer.output_mask!=((1u<<(m-1))-1u))observer.failed=1;
                else ++observer.steady_pairs;
            }
        }
        observer.output_mask=0;memcpy(observer.previous,observer.mapped,4);
    }
    }
    ++observer.interval_calls;++observer.total_calls;
    VkPresentInfoKHR wrapped=*info;
    wrapped.waitSemaphoreCount=1;wrapped.pWaitSemaphores=&observer.ready[index];
    return observer.lower(queue,&wrapped);
}
static int observer_start(VkInstance instance,VkPhysicalDevice physical,VkDevice device,
        VkQueue queue,VkSwapchainKHR swapchain,VkExtent2D extent,VkFormat format,
        VkImage *images,uint32_t count,uint32_t family,PFN_vkQueuePresentKHR lower){
    observer.device=device;observer.queue=queue;observer.images=images;
    observer.image_count=count;observer.extent=extent;observer.lower=lower;
    VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=(VkDeviceSize)extent.width*extent.height*4,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(OBS(CreateBuffer)(device,&bci,NULL,&observer.buffer)!=VK_SUCCESS) return 0;
    VkMemoryRequirements req;OBS(GetBufferMemoryRequirements)(device,observer.buffer,&req);
    VkPhysicalDeviceMemoryProperties memory;
#ifdef GFG_NATIVE_TEST_OBSERVE_LAYER
    memory=observer.memory_props;
#else
    OBS(GetPhysicalDeviceMemoryProperties)(physical,&memory);
#endif
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;++i)
        if((req.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
    if(type==UINT32_MAX) return 0;
    VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    if(OBS(AllocateMemory)(device,&mai,NULL,&observer.memory)!=VK_SUCCESS ||
       OBS(BindBufferMemory)(device,observer.buffer,observer.memory,0)!=VK_SUCCESS ||
       OBS(MapMemory)(device,observer.memory,0,VK_WHOLE_SIZE,0,(void **)&observer.mapped)!=VK_SUCCESS) return 0;
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    if(OBS(CreateCommandPool)(device,&pci,NULL,&observer.pool)!=VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=observer.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(OBS(AllocateCommandBuffers)(device,&ai,&observer.command)!=VK_SUCCESS) return 0;
#ifdef GFG_NATIVE_TEST_OBSERVE_LAYER
    if(observer_set_loader_data && observer_set_loader_data(device,observer.command)!=VK_SUCCESS) return 0;
#endif
    VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if(OBS(CreateFence)(device,&fci,NULL,&observer.fence)!=VK_SUCCESS) return 0;
    observer.ready=calloc(count,sizeof(*observer.ready));if(!observer.ready) return 0;
    VkSemaphoreCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(uint32_t i=0;i<count;++i)
        if(OBS(CreateSemaphore)(device,&sci,NULL,&observer.ready[i])!=VK_SUCCESS) return 0;
#ifndef GFG_NATIVE_TEST_OBSERVE_LAYER
    const gfg_gpu_fg_create_info info={.instance=instance,.gipa=vkGetInstanceProcAddr,
        .physical=physical,.device=device,.gdpa=vkGetDeviceProcAddr,.swapchain=swapchain,
        .extent=extent,.format=format,.present_mode=VK_PRESENT_MODE_FIFO_KHR,
        .queue=queue,.family=family,.set_loader_data=observer_loader_data};
    observer.fg=gfg_gpu_fg_create(&info);
    return observer.fg!=NULL;
#else
    (void)instance; (void)physical; (void)format; (void)swapchain; (void)observer_loader_data;
    return 1;
#endif
}
static int observer_finish(void){
    const char *fault=getenv("GFG_NATIVE_GPU_FAULT");
    const int recovery=fault && !strcmp(fault,"record");
    const int cut=getenv("GFG_NATIVE_TEST_CUT")!=NULL;
    const unsigned m=observer.multiplier?observer.multiplier:3;
    unsigned expected_real=getenv("GFG_NATIVE_TEST_FRAMES")?(unsigned)atoi(getenv("GFG_NATIVE_TEST_FRAMES")):4;
    const int duplicate=getenv("GFG_NATIVE_TEST_DUPLICATE")!=NULL;
    int passed=!observer.failed && (recovery?observer.total_calls==5:
        observer.real_calls==expected_real && ((cut||duplicate)?observer.generated==0:
        observer.steady_pairs==expected_real-2 && observer.generated>=(expected_real-2)*(m-1)));
    const char *algorithm=getenv("GFG_NATIVE_ALGO");
    if(algorithm && !strcmp(algorithm,"gsf") && getenv("GFG_NATIVE_ASYNC") && !recovery && !cut && !duplicate) {
        const char *rate=getenv("GFG_NATIVE_OUTPUT_HZ");double hz=rate?atof(rate):90.0,min_ms=1e9;
        for(unsigned i=1;i<observer.total_calls && i<16;++i) {
            double delta=(double)(observer.handoff_ns[i]-observer.handoff_ns[i-1])/1e6;
            if(delta<min_ms)min_ms=delta;
        }
        int spaced=min_ms>=750.0/hz;
        if(!spaced)passed=0;
        fprintf(stderr,"GPU WSI observer: configured_output_hz=%.3f host_handoff_min_ms=%.3f spacing=%s physical_scanout=unobserved\n",hz,min_ms,spaced?"PASS":"FAIL");
    }
#ifndef GFG_NATIVE_TEST_OBSERVE_LAYER
    gfg_gpu_fg_destroy(observer.fg);
#endif
    for(uint32_t i=0;i<observer.image_count;++i)OBS(DestroySemaphore)(observer.device,observer.ready[i],NULL);
    free(observer.ready);OBS(DestroyFence)(observer.device,observer.fence,NULL);
    OBS(DestroyCommandPool)(observer.device,observer.pool,NULL);
    OBS(UnmapMemory)(observer.device,observer.memory);
    OBS(DestroyBuffer)(observer.device,observer.buffer,NULL);OBS(FreeMemory)(observer.device,observer.memory,NULL);
    if(duplicate)fprintf(stderr,"GPU WSI observer: duplicate_recovery=%s real=%u synthetic=%u physical_scanout=unobserved\n",passed?"PASS":"FAIL",observer.real_calls,observer.generated);
    else if(cut)fprintf(stderr,"GPU WSI observer: real=4 synthetic=0 cut_recovery=%s physical_scanout=unobserved\n",passed?"PASS":"FAIL");
    else if(recovery)
        fprintf(stderr,"GPU WSI observer: real=4 synthetic=0 recovery_duplicate=1 content=%s real_handoffs=PASS physical_scanout=unobserved\n",passed?"PASS":"FAIL");
    else fprintf(stderr,"GPU WSI observer: real=%u synthetic=%u content=%s handoff_order=%s steady_pairs=%u multiplier=%u physical_scanout=unobserved\n",
        observer.real_calls,observer.generated,passed?"PASS":"FAIL",passed?"PASS":"FAIL",observer.steady_pairs,m);
    return passed;
}

#undef OBS

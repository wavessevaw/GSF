/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only Vulkan real source GPU image -> owned presentation swapchain.
 * Two source images emulate snapshots of consecutive game frames. The
 * composite half-A/half-B frame is a visible deterministic validation mark,
 * NOT temporal interpolation. They NEVER have their own WSI presentation.
 * Readback verifies actual GPU pixels just before vkQueuePresentKHR.
 */
#ifndef GFG_OWNED_SOURCE_TRANSPORT_H
#define GFG_OWNED_SOURCE_TRANSPORT_H
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/virtual_layouts.h"
#include "../src/offscreen_source_pool.h"

typedef struct {
    VkDevice d;
    VkExtent2D size;
    VkImage frames[2]; /* independent GSF history snapshots */
    VkDeviceMemory mem[2];
    VkImage game_frames[GFG_OFFSCREEN_MAX_IMAGES]; /* virtual game render targets */
    VkDeviceMemory game_mem[GFG_OFFSCREEN_MAX_IMAGES];
    int external_game_images; /* owned by output proxy, never double-freed */
    unsigned app_image_count; /* 1 or 2: independent of GSF history count */
    uint64_t ids[2], game_ids[GFG_OFFSCREEN_MAX_IMAGES];
    VkBuffer readback;
    VkDeviceMemory readback_mem;
    uint8_t *mapped;
    VkDeviceSize bytes;
    unsigned verified_real,verified_synthetic;
    uint64_t virtual_present_old_rewrites,virtual_present_new_rewrites;
} gfg_source_probe;
static uint32_t gfg_src_mem_type(const VkPhysicalDeviceMemoryProperties *m,
                                  uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<m->memoryTypeCount;i++)
        if((bits&(1u<<i)) &&
           (m->memoryTypes[i].propertyFlags&flags)==flags)return i;
    return UINT32_MAX;
}
static int gfg_source_probe_create(gfg_source_probe *p,
        VkPhysicalDevice phys,VkDevice device,VkExtent2D size,VkFormat format) {
    memset(p,0,sizeof(*p));p->d=device;p->size=size;
    p->app_image_count=2;
    p->bytes=(VkDeviceSize)size.width*size.height*4;
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys,&mem_props);
    for(unsigned i=0;i<4;i++) {
        /* Four allocations: two images for the application and two
         * separate images for GSF history. Never alias their memory. */
        VkImage *image=i<2?&p->frames[i]:&p->game_frames[i-2];
        VkDeviceMemory *allocation=i<2?&p->mem[i]:&p->game_mem[i-2];
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType=VK_IMAGE_TYPE_2D,.format=format,
          .extent={size.width,size.height,1},.mipLevels=1,.arrayLayers=1,
          .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
          .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          .sharingMode=VK_SHARING_MODE_EXCLUSIVE,
          .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
        if(vkCreateImage(device,&ci,NULL,image)!=VK_SUCCESS)return 0;
        VkMemoryRequirements req;vkGetImageMemoryRequirements(device,*image,&req);
        uint32_t type=gfg_src_mem_type(&mem_props,req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(type==UINT32_MAX)type=gfg_src_mem_type(&mem_props,req.memoryTypeBits,0);
        if(type==UINT32_MAX)return 0;
        VkMemoryAllocateInfo a={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                 .allocationSize=req.size,.memoryTypeIndex=type};
        if(vkAllocateMemory(device,&a,NULL,allocation)!=VK_SUCCESS ||
           vkBindImageMemory(device,*image,*allocation,0)!=VK_SUCCESS)return 0;
    }
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=p->bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(vkCreateBuffer(device,&bc,NULL,&p->readback)!=VK_SUCCESS)return 0;
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,p->readback,&req);
    const uint32_t mt=gfg_src_mem_type(&mem_props,req.memoryTypeBits,
       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(mt==UINT32_MAX)return 0;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=req.size,.memoryTypeIndex=mt};
    if(vkAllocateMemory(device,&ma,NULL,&p->readback_mem)!=VK_SUCCESS ||
       vkBindBufferMemory(device,p->readback,p->readback_mem,0)!=VK_SUCCESS ||
       vkMapMemory(device,p->readback_mem,0,VK_WHOLE_SIZE,0,(void**)&p->mapped)!=VK_SUCCESS)
        return 0;
    return 1;
}
static void gfg_source_probe_destroy(gfg_source_probe *p) {
    if(p->mapped)vkUnmapMemory(p->d,p->readback_mem);
    if(p->readback)vkDestroyBuffer(p->d,p->readback,NULL);
    if(p->readback_mem)vkFreeMemory(p->d,p->readback_mem,NULL);
    for(unsigned i=0;i<2;i++) {
        if(p->frames[i])vkDestroyImage(p->d,p->frames[i],NULL);
        if(p->mem[i])vkFreeMemory(p->d,p->mem[i],NULL);
    }
    if(!p->external_game_images)for(unsigned i=0;i<GFG_OFFSCREEN_MAX_IMAGES;i++) {
        if(p->game_frames[i])vkDestroyImage(p->d,p->game_frames[i],NULL);
        if(p->game_mem[i])vkFreeMemory(p->d,p->game_mem[i],NULL);
    }
}
/* The real Vulkan-backed proxy now owns the app frames, replacing only
 * the test-created app targets. History and readback remain independent. */
static int gfg_source_probe_adopt_app_pool(gfg_source_probe *p,
                                     const gfg_offscreen_source_pool *pool) {
    if(!p || !pool || !pool->count ||
       pool->count>GFG_OFFSCREEN_MAX_IMAGES || p->external_game_images ||
       p->verified_real || p->verified_synthetic)return 0;
    for(unsigned i=0;i<2;i++) {
        if(p->game_frames[i])vkDestroyImage(p->d,p->game_frames[i],NULL);
        if(p->game_mem[i])vkFreeMemory(p->d,p->game_mem[i],NULL);
    }
    memset(p->game_frames,0,sizeof(p->game_frames));
    memset(p->game_mem,0,sizeof(p->game_mem));
    for(unsigned i=0;i<pool->count;++i) {
        p->game_frames[i]=pool->images[i];
        p->game_mem[i]=pool->memory[i];
    }
    p->app_image_count=pool->count;
    p->external_game_images=1;
    return 1;
}
static VkImageMemoryBarrier gfg_src_barrier(VkImage img,VkImageLayout from,
                 VkImageLayout to,VkAccessFlags before,VkAccessFlags after) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=before,.dstAccessMask=after,
        .oldLayout=from,.newLayout=to,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=img,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    return b;
}
static float gfg_source_level(uint64_t id) {
    return 0.15f+0.11f*(float)(id%5u);
}
/* 1. Game command buffer renders into its OWN offscreen image. */
static void gfg_source_record_game(gfg_source_probe *p,VkCommandBuffer cmd,
                                   uint64_t id) {
    const unsigned slot=(unsigned)(id%p->app_image_count);
    VkImage image=p->game_frames[slot];
    /* A real game regards the returned virtual image as a swapchain image.
     * Rewrites of PRESENT_SRC happen before the ICD observes this command. */
    gfg_virtual_layout_map layout_map={.images=p->game_frames,
                                        .count=p->app_image_count};
    VkImageMemoryBarrier requested_pre=gfg_src_barrier(image,
        p->game_ids[slot]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        p->game_ids[slot]?VK_ACCESS_TRANSFER_READ_BIT:0,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageMemoryBarrier pre={0};
    gfg_vlayout_legacy(&layout_map,&requested_pre,&pre);
    vkCmdPipelineBarrier(cmd,p->game_ids[slot]?VK_PIPELINE_STAGE_TRANSFER_BIT:
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&pre);
    const float level=gfg_source_level(id);
    VkClearColorValue c={.float32={level,level,level,1.0f}};
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&c,1,&range);
    VkImageMemoryBarrier requested_post=gfg_src_barrier(image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkImageMemoryBarrier post={0};
    gfg_vlayout_legacy(&layout_map,&requested_post,&post);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&post);
    p->virtual_present_old_rewrites+=layout_map.translated_old;
    p->virtual_present_new_rewrites+=layout_map.translated_new;
    p->game_ids[slot]=id;
}
/* 2. GSF capture command buffer waits for the game's render fence before
 * copying the finished app image into an INDEPENDENT history allocation.
 * Once this transfer completes the game slot can be reacquired/reused.
 * The output stage must never read game_frames[]. */
static void gfg_source_record_snapshot(gfg_source_probe *p,VkCommandBuffer cmd,
                                        uint64_t id) {
    const unsigned slot=(unsigned)(id&1u);
    const unsigned app_slot=(unsigned)(id%p->app_image_count);
    VkImage target=p->frames[slot];
    VkImageMemoryBarrier pre=gfg_src_barrier(target,
        p->ids[slot]?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        p->ids[slot]?VK_ACCESS_TRANSFER_READ_BIT:0,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd,p->ids[slot]?VK_PIPELINE_STAGE_TRANSFER_BIT:
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&pre);
    VkImageCopy region={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .extent={p->size.width,p->size.height,1}};
    vkCmdCopyImage(cmd,p->game_frames[app_slot],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    VkImageMemoryBarrier post=gfg_src_barrier(target,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&post);
    p->ids[slot]=id;
}
/* The SOURCE has its own submission, preceding owner presentation.
 * Never invent/render source data from inside the output present path. */
static int gfg_source_snapshot_ready(const gfg_source_probe *p,
                                      uint64_t a,uint64_t b) {
    return a && b && p->ids[a&1u]==a && p->ids[b&1u]==b;
}
/* Writes previously rendered GPU source pixels to the ONE output swapchain.
 * Intermediate test marker is split A/B composition (not real FG).
 * Caller must wait for its source capture fence or establish a GPU
 * semaphore dependency before submitting this output. */
static void gfg_source_record_output(gfg_source_probe *p,VkCommandBuffer cmd,
                VkImage dst,uint64_t a,uint64_t b,int synthetic) {
    if(synthetic) {
        const uint32_t left=p->size.width/2;
        VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
             .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
             .extent={left,p->size.height,1}};
        vkCmdCopyImage(cmd,p->frames[a&1u],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        copy.srcOffset.x=(int32_t)left;
        copy.dstOffset.x=(int32_t)left;
        copy.extent.width=p->size.width-left;
        vkCmdCopyImage(cmd,p->frames[b&1u],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    } else {
        VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
             .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
             .extent={p->size.width,p->size.height,1}};
        vkCmdCopyImage(cmd,p->frames[a&1u],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    }
    VkImageMemoryBarrier post=gfg_src_barrier(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&post);
    VkBufferImageCopy region={.bufferOffset=0,.bufferRowLength=0,
        .bufferImageHeight=0,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={p->size.width,p->size.height,1}};
    vkCmdCopyImageToBuffer(cmd,dst,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        p->readback,1,&region);
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=p->readback,.offset=0,.size=VK_WHOLE_SIZE};
    VkImageMemoryBarrier present=gfg_src_barrier(dst,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         VK_ACCESS_TRANSFER_READ_BIT,0);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
         0,0,NULL,1,&host,1,&present);
}
static int gfg_source_verify(gfg_source_probe *p,uint64_t a,uint64_t b,
                             int synthetic) {
    const uint32_t xs[2]={p->size.width/4,3*p->size.width/4};
    const uint32_t y=p->size.height/2;
    for(unsigned side=0;side<2;side++) {
        const uint64_t expected_id=synthetic && side?b:a;
        const int expected=(int)(gfg_source_level(expected_id)*255+0.5f);
        const size_t index=((size_t)y*p->size.width+xs[side])*4;
        for(unsigned c=0;c<3;c++) {
            const int found=p->mapped[index+c];
            if(found-expected>2 || expected-found>2)return 0;
        }
    }
    if(synthetic)++p->verified_synthetic;
    else ++p->verified_real;
    return 1;
}
#endif

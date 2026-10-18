/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real WSI smoke: two coloured *real* frames, XCB surface under Xvfb.
 * Exercises Vulkan acquire / barriers / submit / queue-present / retirement.
 * This does not claim TE generation or actual OLED scanout.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include <xcb/xcb.h>

#ifdef GFG_NATIVE_TEST_SMALL
#define TEST_WIDTH 96u
#define TEST_HEIGHT 64u
#else
#define TEST_WIDTH 640u
#define TEST_HEIGHT 360u
#endif

#define TRY(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS && r_ != VK_SUBOPTIMAL_KHR) { \
  fprintf(stderr, "Vulkan failure %s => %d at %d\n", #x, r_, __LINE__); return 1; \
} } while (0)
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "Requirement failed: %s line %d\n", #x, __LINE__); return 1; } } while (0)

static int supports(VkPhysicalDevice physical, const char *ext) {
    uint32_t count=0;
    if (vkEnumerateDeviceExtensionProperties(physical,NULL,&count,NULL)!=VK_SUCCESS) return 0;
    VkExtensionProperties *p=calloc(count,sizeof(*p));
    if (!p) return 0;
    int yes=0;
    if (vkEnumerateDeviceExtensionProperties(physical,NULL,&count,p)==VK_SUCCESS)
        for (uint32_t k=0;k<count;k++) if (!strcmp(ext,p[k].extensionName)) {yes=1;break;}
    free(p);
    return yes;
}
static uint32_t between(uint32_t x,uint32_t low,uint32_t high) {
    if(x<low) return low;
    return x>high?high:x;
}

int main(void) {
    int screen_no=0;
    xcb_connection_t *xc=xcb_connect(NULL,&screen_no);
    REQUIRE(xc && !xcb_connection_has_error(xc));
    const xcb_setup_t *setup=xcb_get_setup(xc);
    xcb_screen_iterator_t it=xcb_setup_roots_iterator(setup);
    for(int k=0;k<screen_no;k++) xcb_screen_next(&it);
    REQUIRE(it.data);
    xcb_screen_t *screen=it.data;
    xcb_window_t window=xcb_generate_id(xc);
    const uint32_t opts[]={screen->black_pixel,XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(xc,XCB_COPY_FROM_PARENT,window,screen->root,0,0,TEST_WIDTH,TEST_HEIGHT,0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,screen->root_visual,
        XCB_CW_BACK_PIXEL|XCB_CW_EVENT_MASK,opts);
    xcb_map_window(xc,window);
    REQUIRE(xcb_flush(xc)>0);

    const char *instance_extensions[]={"VK_KHR_surface","VK_KHR_xcb_surface"};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="gfg-native-wsi-smoke",
        .pEngineName="gfg-native-test",
        .apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&app,.enabledExtensionCount=2,
        .ppEnabledExtensionNames=instance_extensions};
    VkInstance instance=VK_NULL_HANDLE;
    TRY(vkCreateInstance(&ci,NULL,&instance));
    VkXcbSurfaceCreateInfoKHR sci={.sType=VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection=xc,.window=window};
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    TRY(vkCreateXcbSurfaceKHR(instance,&sci,NULL,&surface));

    uint32_t count=0;
    TRY(vkEnumeratePhysicalDevices(instance,&count,NULL));
    REQUIRE(count>0);
    VkPhysicalDevice *phys=calloc(count,sizeof(*phys));
    REQUIRE(phys);
    TRY(vkEnumeratePhysicalDevices(instance,&count,phys));
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<count && !physical;i++) {
        if(!supports(phys[i],"VK_KHR_swapchain")) continue;
        uint32_t qcount=0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i],&qcount,NULL);
        VkQueueFamilyProperties *families=calloc(qcount,sizeof(*families));
        REQUIRE(families);
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i],&qcount,families);
        for(uint32_t q=0;q<qcount;q++){
            VkBool32 presented=VK_FALSE;
            if(families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                TRY(vkGetPhysicalDeviceSurfaceSupportKHR(phys[i],q,surface,&presented));
            if(presented){physical=phys[i];family=q;break;}
        }
        free(families);
    }
    free(phys);
    REQUIRE(physical && family!=UINT32_MAX);
    const float priority=1.0f;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *device_ext[]={"VK_KHR_swapchain"};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=device_ext};
    VkDevice device=VK_NULL_HANDLE;
    TRY(vkCreateDevice(physical,&di,NULL,&device));
    VkQueue queue=VK_NULL_HANDLE;
    vkGetDeviceQueue(device,family,0,&queue);
    REQUIRE(queue);

    VkSurfaceCapabilitiesKHR caps;
    TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));
    REQUIRE(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkExtent2D ext=caps.currentExtent;
    if(ext.width==UINT32_MAX) {
        ext.width=between(TEST_WIDTH,caps.minImageExtent.width,caps.maxImageExtent.width);
        ext.height=between(TEST_HEIGHT,caps.minImageExtent.height,caps.maxImageExtent.height);
    }
    uint32_t nformats=0;
    TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&nformats,NULL));
    REQUIRE(nformats>0);
    VkSurfaceFormatKHR *formats=calloc(nformats,sizeof(*formats)); REQUIRE(formats);
    TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&nformats,formats));
    VkSurfaceFormatKHR color=formats[0];
    const char *want_srgb=getenv("GFG_NATIVE_TEST_SRGB");
    const int srgb=want_srgb && !strcmp(want_srgb,"1");
    const VkFormat wanted=srgb?VK_FORMAT_B8G8R8A8_SRGB:VK_FORMAT_B8G8R8A8_UNORM;
    int found=0;
    for(uint32_t i=0;i<nformats;i++)if(formats[i].format==wanted)
        {color=formats[i];found=1;break;}
    free(formats);
    if(srgb) REQUIRE(found);
    if(srgb)fprintf(stderr,"GFG native: WSI SRGB format=%d selected\n",(int)color.format);
    uint32_t minimum=caps.minImageCount<2?2:caps.minImageCount;
    if(caps.maxImageCount && minimum>caps.maxImageCount) minimum=caps.maxImageCount;
    VkSwapchainCreateInfoKHR swci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface=surface,.minImageCount=minimum,.imageFormat=color.format,
        .imageColorSpace=color.colorSpace,.imageExtent=ext,.imageArrayLayers=1,
        .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
        .preTransform=caps.currentTransform,.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    if (!(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        for(uint32_t bit=1;bit<=8;bit*=2) if(caps.supportedCompositeAlpha&bit)
            {swci.compositeAlpha=(VkCompositeAlphaFlagBitsKHR)bit;break;}
    }
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    TRY(vkCreateSwapchainKHR(device,&swci,NULL,&swapchain));

    uint32_t image_count=0;
    TRY(vkGetSwapchainImagesKHR(device,swapchain,&image_count,NULL));
    REQUIRE(image_count>=minimum);
    VkImage *images=calloc(image_count,sizeof(*images));REQUIRE(images);
    TRY(vkGetSwapchainImagesKHR(device,swapchain,&image_count,images));
    VkCommandPoolCreateInfo poolci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex=family,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool=VK_NULL_HANDLE;
    TRY(vkCreateCommandPool(device,&poolci,NULL,&pool));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd=VK_NULL_HANDLE; TRY(vkAllocateCommandBuffers(device,&cbai,&cmd));
    VkSemaphoreCreateInfo semci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired=VK_NULL_HANDLE, ready=VK_NULL_HANDLE;
    TRY(vkCreateSemaphore(device,&semci,NULL,&acquired));
    TRY(vkCreateSemaphore(device,&semci,NULL,&ready));
    unsigned char *seen=calloc(image_count,1);REQUIRE(seen);

    PFN_vkQueuePresentKHR present_gipa=(PFN_vkQueuePresentKHR)
        vkGetInstanceProcAddr(instance,"vkQueuePresentKHR");
    PFN_vkQueuePresentKHR present_gdpa=(PFN_vkQueuePresentKHR)
        vkGetDeviceProcAddr(device,"vkQueuePresentKHR");
    REQUIRE(present_gipa && present_gdpa);
    for(uint32_t frame=0;frame<4;frame++) {
        uint32_t index=UINT32_MAX;
        TRY(vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index));
        REQUIRE(index<image_count);
        TRY(vkResetCommandBuffer(cmd,0));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(cmd,&begin));
        VkImageMemoryBarrier pre={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=seen[index]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=images[index],
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&pre);
        VkClearColorValue clear={.float32={0.20f+0.05f*frame,0.2f,0.80f-0.05f*frame,1}};
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
        VkImageMemoryBarrier post=pre;
        post.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        post.dstAccessMask=0;
        post.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&post);
        TRY(vkEndCommandBuffer(cmd));
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount=1,.pWaitSemaphores=&acquired,.pWaitDstStageMask=&stage,
            .commandBufferCount=1,.pCommandBuffers=&cmd,
            .signalSemaphoreCount=1,.pSignalSemaphores=&ready};
        TRY(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));
        VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount=1,.pWaitSemaphores=&ready,
            .swapchainCount=1,.pSwapchains=&swapchain,.pImageIndices=&index};
        if(frame%2==0) TRY(present_gipa(queue,&pi));
        else TRY(present_gdpa(queue,&pi));
        TRY(vkQueueWaitIdle(queue));
        seen[index]=1;
    }
    free(seen);
    vkDestroySemaphore(device,ready,NULL);
    vkDestroySemaphore(device,acquired,NULL);
    vkDestroyCommandPool(device,pool,NULL);
    free(images);
    vkDestroySwapchainKHR(device,swapchain,NULL);
    vkDestroyDevice(device,NULL);
    vkDestroySurfaceKHR(instance,surface,NULL);
    vkDestroyInstance(instance,NULL);
    xcb_destroy_window(xc,window);xcb_disconnect(xc);
    puts("GFG native: 4 real WSI acquire/submit/present cycles PASS");
    return 0;
}

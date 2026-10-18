/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real Vulkan Loader + XCB + lavapipe CREATE-ONLY virtual swapchain test.
 *
 * The test application does not inject GFG native-layer handles/callbacks.
 * It deliberately does NOT acquire, render, or present its virtual WSI.
 * That requires a separate, unimplemented asynchronous runtime.
 */
#define _POSIX_C_SOURCE 200809L
#ifndef VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include "../src/offscreen_source_pool.h"
#include <xcb/xcb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr,"GSF loader probe assertion: %s line %d\\n",#x,__LINE__); \
    return 1; } } while(0)
#define TRY(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS) { \
    fprintf(stderr,"GSF loader probe Vulkan error %s: %d line %d\\n",#x,r_,__LINE__); \
    return 1; } } while(0)

int main(void) {
    REQUIRE(getenv("GFG_NATIVE_TEST_LOADER_CREATE_PROBE"));
    REQUIRE(getenv("GFG_NATIVE_TEST_PROXY_CREATE"));
    xcb_connection_t *x=xcb_connect(NULL,NULL);
    REQUIRE(x && !xcb_connection_has_error(x));
    const xcb_setup_t *setup=xcb_get_setup(x);
    xcb_screen_t *screen=xcb_setup_roots_iterator(setup).data;
    REQUIRE(screen);
    const xcb_window_t window=xcb_generate_id(x);
    const uint32_t attributes[1]={screen->black_pixel};
    xcb_create_window(x,XCB_COPY_FROM_PARENT,window,screen->root,
        0,0,96,64,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,XCB_CW_BACK_PIXEL,attributes);
    xcb_map_window(x,window);
    REQUIRE(xcb_flush(x)>0);

    const char *instance_extensions[]={"VK_KHR_surface","VK_KHR_xcb_surface"};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="gsf-loader-proxy-create-only",
        .pEngineName="gfg-output-loader-probe",.apiVersion=VK_API_VERSION_1_2};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&app,.enabledExtensionCount=2,
        .ppEnabledExtensionNames=instance_extensions};
    VkInstance instance=VK_NULL_HANDLE;
    TRY(vkCreateInstance(&ic,NULL,&instance));
    VkXcbSurfaceCreateInfoKHR xc={.sType=VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection=x,.window=window};
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    TRY(vkCreateXcbSurfaceKHR(instance,&xc,NULL,&surface));

    uint32_t phys_count=0;TRY(vkEnumeratePhysicalDevices(instance,&phys_count,NULL));
    REQUIRE(phys_count && phys_count<64);
    VkPhysicalDevice *physical_list=calloc(phys_count,sizeof(*physical_list));
    REQUIRE(physical_list);
    TRY(vkEnumeratePhysicalDevices(instance,&phys_count,physical_list));
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<phys_count && !physical;i++) {
        uint32_t families=0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_list[i],&families,NULL);
        VkQueueFamilyProperties *props=calloc(families,sizeof(*props));
        REQUIRE(props);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_list[i],&families,props);
        for(uint32_t j=0;j<families;j++) {
            VkBool32 supported=VK_FALSE;
            if(props[j].queueFlags&VK_QUEUE_GRAPHICS_BIT)
                TRY(vkGetPhysicalDeviceSurfaceSupportKHR(physical_list[i],j,
                                                         surface,&supported));
            if(supported){physical=physical_list[i];family=j;break;}
        }
        free(props);
    }
    free(physical_list);
    REQUIRE(physical && family!=UINT32_MAX);
    const float priority=1.0f;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *device_extensions[]={"VK_KHR_swapchain"};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=device_extensions};
    VkDevice device=VK_NULL_HANDLE;
    TRY(vkCreateDevice(physical,&dc,NULL,&device));
    VkSurfaceCapabilitiesKHR caps={0};
    TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));
    const VkImageUsageFlags needed=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    REQUIRE((caps.supportedUsageFlags & needed)==needed);
    uint32_t nformat=0;TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&nformat,NULL));
    REQUIRE(nformat && nformat<512);
    VkSurfaceFormatKHR *formats=calloc(nformat,sizeof(*formats));
    REQUIRE(formats);
    TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&nformat,formats));
    VkSurfaceFormatKHR fmt={0};
    for(uint32_t i=0;i<nformat;i++)
        if(formats[i].format==VK_FORMAT_B8G8R8A8_UNORM &&
           formats[i].colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt=formats[i];break;
        }
    free(formats);
    REQUIRE(fmt.format==VK_FORMAT_B8G8R8A8_UNORM);
    uint32_t count=caps.minImageCount<2?2:caps.minImageCount;
    REQUIRE(count<=GFG_OFFSCREEN_MAX_IMAGES);
    REQUIRE(!caps.maxImageCount || count<=caps.maxImageCount);
    VkExtent2D extent=caps.currentExtent;
    if(extent.width==UINT32_MAX)extent=(VkExtent2D){96,64};
    VkSwapchainCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface=surface,.minImageCount=count,
        .imageFormat=fmt.format,.imageColorSpace=fmt.colorSpace,
        .imageExtent=extent,.imageArrayLayers=1,.imageUsage=needed,
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
        .preTransform=caps.currentTransform,
        .compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    if(!(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        for(unsigned bit=1;bit<=8;bit*=2)
            if(caps.supportedCompositeAlpha&bit) {
                ci.compositeAlpha=(VkCompositeAlphaFlagBitsKHR)bit;break;
            }

    VkSwapchainKHR virtual_handle=VK_NULL_HANDLE;
    TRY(vkCreateSwapchainKHR(device,&ci,NULL,&virtual_handle));
    REQUIRE(virtual_handle);
    uint32_t images_count=0;
    TRY(vkGetSwapchainImagesKHR(device,virtual_handle,&images_count,NULL));
    REQUIRE(images_count==count);
    VkImage images[GFG_OFFSCREEN_MAX_IMAGES]={0};
    TRY(vkGetSwapchainImagesKHR(device,virtual_handle,&images_count,images));
    for(uint32_t i=0;i<images_count;i++) {
        REQUIRE(images[i]!=VK_NULL_HANDLE);
        for(uint32_t j=0;j<i;j++)REQUIRE(images[i]!=images[j]);
    }
    fprintf(stderr,"GSF_LOADER_APP virtual_images=%u real_driver_allocations=1 PASS\\n",
            images_count);
    vkDestroySwapchainKHR(device,virtual_handle,NULL);

    /* Confirm opt-out preserves the game's unmodified real WSI. */
    REQUIRE(setenv("GFG_NATIVE_TEST_PROXY_CREATE","0",1)==0);
    VkSwapchainKHR real_handle=VK_NULL_HANDLE;
    TRY(vkCreateSwapchainKHR(device,&ci,NULL,&real_handle));
    REQUIRE(real_handle);
    uint32_t real_count=0;
    TRY(vkGetSwapchainImagesKHR(device,real_handle,&real_count,NULL));
    REQUIRE(real_count>=count);
    vkDestroySwapchainKHR(device,real_handle,NULL);
    fprintf(stderr,"GSF_LOADER_APP optout_passthrough=1 native_swapchain_destroy=1 PASS\\n");

    vkDestroyDevice(device,NULL);
    vkDestroySurfaceKHR(instance,surface,NULL);
    vkDestroyInstance(instance,NULL);
    xcb_destroy_window(x,window);xcb_disconnect(x);
    puts("GSF loader-installed Vulkan app CREATE/GETIMAGES/DESTROY PASS (no present/Deck)");
    return 0;
}

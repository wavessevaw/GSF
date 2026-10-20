/* SPDX-License-Identifier: GPL-3.0-or-later
 * Loader-installed virtual WSI: GPU render -> layer capture worker -> real
 * compositor output -> X11 pixel check. Synthetic colors only, not FG.
 */
#define _POSIX_C_SOURCE 200809L
#ifndef VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include "../src/offscreen_source_pool.h"
#include "x11_output_probe.h"
#include "test_wsi_results.h"
#include "../src/output_frame_pacer.h"
#include <xcb/xcb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr,"GSF loader probe assertion: %s line %d\\n",#x,__LINE__); \
    return 1; } } while(0)
#define TRY(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS) { \
    fprintf(stderr,"GSF loader probe Vulkan error %s: %d line %d\\n",#x,r_,__LINE__); \
    return 1; } } while(0)

int main(void) {
    REQUIRE(getenv("GFG_NATIVE_TEST_LOADER_LIVE_OUTPUT"));
    REQUIRE(getenv("GFG_NATIVE_TEST_PROXY_CREATE"));
    xcb_connection_t *x=xcb_connect(NULL,NULL);
    REQUIRE(x && !xcb_connection_has_error(x));
    const xcb_setup_t *setup=xcb_get_setup(x);
    xcb_screen_t *screen=xcb_setup_roots_iterator(setup).data;
    REQUIRE(screen);
    /* CI keeps a 96x64 window. An opt-in Deck test can create a window
     * sized to the actual 1280x800 internal display (without root/system
     * settings), allowing a human to see split frames on screen. */
    uint16_t demo_w=96,demo_h=64;
    const char *w_env=getenv("GFG_NATIVE_TEST_WINDOW_WIDTH");
    const char *h_env=getenv("GFG_NATIVE_TEST_WINDOW_HEIGHT");
    if(w_env || h_env) {
        REQUIRE(w_env && h_env);
        char *w_end=NULL,*h_end=NULL;
        const unsigned long w=strtoul(w_env,&w_end,10);
        const unsigned long h=strtoul(h_env,&h_end,10);
        REQUIRE(w_env!=w_end && h_env!=h_end &&
                w_end && !*w_end && h_end && !*h_end);
        REQUIRE(w>=64 && w<=1920 && !(w&1u) && h>=64 && h<=1200);
        demo_w=(uint16_t)w;demo_h=(uint16_t)h;
    }
    const xcb_window_t window=xcb_generate_id(x);
    const uint32_t attributes[1]={screen->black_pixel};
    xcb_create_window(x,XCB_COPY_FROM_PARENT,window,screen->root,
        0,0,demo_w,demo_h,0,XCB_WINDOW_CLASS_INPUT_OUTPUT,
        screen->root_visual,XCB_CW_BACK_PIXEL,attributes);
    xcb_map_window(x,window);
    REQUIRE(xcb_flush(x)>0);

    const char *instance_extensions[]={"VK_KHR_surface","VK_KHR_xcb_surface"};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="gsf-loader-proxy-live-output",
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
    fprintf(stderr,"GSF_DECK_PROBE_WINDOW width=%u height=%u live_loader_test=1\n",
            demo_w,demo_h);

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
    const int test_maintenance1=getenv("GFG_NATIVE_TEST_INJECT_MAINTENANCE1")!=NULL;
    const char *device_extensions[]={"VK_KHR_swapchain",
                                     VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance_feature={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
        .swapchainMaintenance1=VK_TRUE};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,
        .enabledExtensionCount=test_maintenance1?2u:1u,
        .ppEnabledExtensionNames=device_extensions};
    if(test_maintenance1) {
        uint32_t extensions=0;
        TRY(vkEnumerateDeviceExtensionProperties(physical,NULL,&extensions,NULL));
        REQUIRE(extensions>0&&extensions<2048);
        VkExtensionProperties *ep=calloc(extensions,sizeof(*ep));
        REQUIRE(ep);
        TRY(vkEnumerateDeviceExtensionProperties(physical,NULL,&extensions,ep));
        int found=0;
        for(uint32_t k=0;k<extensions;k++)
            if(!strcmp(ep[k].extensionName,
                       VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
                found=1;
        free(ep);
        if(!found) {
            fprintf(stderr,
               "GSF_TEST_APP_MAINTENANCE1_DRIVER_UNAVAILABLE"
               " lavapipe_cannot_validate_real_pnext=1"
               " policy_unit_test_required=1 physical_deck=UNVERIFIED\n");
            return 77; /* SKIP: wrapper CI must recognize this exact exit code. */
        }
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT supported={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext=&supported};
        vkGetPhysicalDeviceFeatures2(physical,&f2);
        REQUIRE(supported.swapchainMaintenance1);
        dc.pNext=&maintenance_feature;
        fprintf(stderr,
                "GSF_TEST_APP_MAINTENANCE1 extension_enabled=1 feature_enabled=1\n");
    }
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
    if(extent.width==UINT32_MAX)extent=(VkExtent2D){demo_w,demo_h};
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

    if(getenv("GFG_NATIVE_TEST_REQUEST_MAILBOX")) {
        uint32_t present_modes_count=0;
        TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,
                                                      &present_modes_count,NULL));
        REQUIRE(present_modes_count && present_modes_count<64);
        VkPresentModeKHR modes[64]={0};
        TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,
                                                      &present_modes_count,modes));
        int mailbox_supported=0;
        for(uint32_t i=0;i<present_modes_count;i++)
            if(modes[i]==VK_PRESENT_MODE_MAILBOX_KHR)mailbox_supported=1;
        REQUIRE(mailbox_supported);
        ci.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
        fprintf(stderr,"GSF_TEST_APP_MAILBOX requested=1 surface_supported=1\n");
    }
    VkPresentModeKHR maintenance_mode=VK_PRESENT_MODE_MAILBOX_KHR;
    const VkSwapchainPresentModesCreateInfoEXT maintenance_ci={
        .sType=VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
        .presentModeCount=1,.pPresentModes=&maintenance_mode};
    if(test_maintenance1) {
        REQUIRE(ci.presentMode==VK_PRESENT_MODE_MAILBOX_KHR);
        ci.pNext=&maintenance_ci;
        fprintf(stderr,
           "GSF_TEST_APP_MAINTENANCE1 injected_pnext=1000275002 allowed_mailbox=1\n");
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
    /* Vulkan app uses the loader and its normal vk* dispatch. No layer
     * device registration or injected callbacks from the test executable. */
    VkQueue queue=VK_NULL_HANDLE;
    vkGetDeviceQueue(device,family,0,&queue);
    REQUIRE(queue);
    VkCommandPool pool=VK_NULL_HANDLE;
    const VkCommandPoolCreateInfo pool_info={
        .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex=family};
    TRY(vkCreateCommandPool(device,&pool_info,NULL,&pool));
    const VkCommandBufferAllocateInfo cb_info={
        .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount=1};
    VkCommandBuffer cmd=VK_NULL_HANDLE;
    TRY(vkAllocateCommandBuffers(device,&cb_info,&cmd));
    const VkSemaphoreCreateInfo sem_info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore image_ready=VK_NULL_HANDLE,render_done=VK_NULL_HANDLE;
    TRY(vkCreateSemaphore(device,&sem_info,NULL,&image_ready));
    TRY(vkCreateSemaphore(device,&sem_info,NULL,&render_done));
    const VkFenceCreateInfo fc={
        .sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags=VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence rendering=VK_NULL_HANDLE;
    TRY(vkCreateFence(device,&fc,NULL,&rendering));
    unsigned char seen[GFG_OFFSCREEN_MAX_IMAGES]={0};
    const int interleave=getenv("GFG_NATIVE_TEST_OUTPUT_INTERLEAVE")!=NULL;
    const int lmc_owner=getenv("GFG_NATIVE_TEST_LMC_OUTPUT_OWNER")!=NULL;
    const unsigned lmc_multiplier=getenv("GFG_NATIVE_TEST_LMC_X4")?4u:
                                  getenv("GFG_NATIVE_TEST_LMC_X3")?3u:2u;
    const unsigned middle_count=getenv("GFG_NATIVE_TEST_POST_EXPECT_REAL_ONLY")?0u:
        lmc_owner && !getenv("GFG_NATIVE_TEST_LMC_MASK2")?lmc_multiplier-1u:1u;
    const int paced_30_60=getenv("GFG_NATIVE_TEST_PACED_30_60")!=NULL;
    const int paced_45_90=getenv("GFG_NATIVE_TEST_PACED_45_90")!=NULL;
    const int paced_225_90=getenv("GFG_NATIVE_TEST_PACED_225_90")!=NULL;
    const int hold_first=getenv("GFG_NATIVE_TEST_PIPELINE_HOLD_FIRST")!=NULL;
    const int paced_source=paced_30_60||paced_45_90||paced_225_90;
    /* Gamescope WSI may present directly without populating X11 backing
     * pixels. Do not treat XCB GetImage black as a Vulkan output failure.
     * Crucially, do NOT label GPU-submit-only evidence as visible pixels. */
    const int gamescope_observer=getenv("GFG_NATIVE_TEST_GAME_MODE")!=NULL;
    const int gpu_only=getenv("GFG_NATIVE_TEST_GPU_READBACK_ONLY")!=NULL;
    const int observe_pixels=!gamescope_observer && !gpu_only;
    REQUIRE(!gpu_only || lmc_owner);
    const int sustained=getenv("GFG_NATIVE_TEST_SUSTAINED")!=NULL;
    const uint32_t frame_count=getenv("GFG_NATIVE_TEST_LMC_VERIFY_DUPLICATE")?2u:sustained?120u:8u;
    const uint32_t source_hz=paced_225_90?22u:paced_45_90?45u:30u;
    const uint32_t output_hz=paced_45_90||paced_225_90?90u:60u;
    /* 22.5 FPS is exactly a 4-tick interval at the 90-Hz output clock. */
    const uint64_t source_period_ns=paced_225_90?4000000000ull/90u:
                                   1000000000ull/source_hz;
    REQUIRE(!paced_source || interleave);
    REQUIRE((int)paced_30_60+(int)paced_45_90+(int)paced_225_90<=1);
    REQUIRE(!paced_225_90 || (lmc_owner && lmc_multiplier==4u));
    REQUIRE(!hold_first || (lmc_owner && interleave && observe_pixels));
    REQUIRE(!sustained || paced_source);
    uint32_t visible=0, intermediate_visible=0, output_visible=0;
    int last_level=-1;
    const uint64_t source_start=gfg_pacer_monotonic_ns();
    REQUIRE(source_start);
    uint64_t x11_last_ns=0,x11_interval_total_ns=0;
    uint64_t x11_interval_min_ns=UINT64_MAX,x11_interval_max_ns=0;
    unsigned x11_interval_count=0;
    for(uint32_t frame=1;frame<=frame_count;frame++) {
        if(paced_source && frame>1) {
            /* Input app and independent Output Engine run separate
             * monotonic clocks at either 30/60 or 45/90 Hz. */
            const uint64_t due=source_start+
                 (uint64_t)(frame-1u)*source_period_ns;
            const struct timespec deadline=gfg_pacer_to_time(due);
            int err=0;
            do {err=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,
                                    &deadline,NULL);}
            while(err==EINTR);
            REQUIRE(err==0);
        }
        uint32_t index=UINT32_MAX;
        VkResult acquired=vkAcquireNextImageKHR(device,virtual_handle,
                                  UINT64_MAX,image_ready,VK_NULL_HANDLE,&index);
        if(!gfg_test_wsi_result_usable(acquired)) {
            fprintf(stderr,"GSF_DECK_WSI acquire_unusable=%d frame=%u recreate_required=%d\n",
                (int)acquired,frame,gfg_test_wsi_recreate_required(acquired));
            return 1;
        }
        if(acquired==VK_SUBOPTIMAL_KHR)
            fprintf(stderr,"GSF_DECK_WSI acquire_suboptimal_usable=1 frame=%u\n",frame);
        REQUIRE(index<images_count);
        TRY(vkWaitForFences(device,1,&rendering,VK_TRUE,UINT64_MAX));
        TRY(vkResetCommandBuffer(cmd,0));
        const VkCommandBufferBeginInfo begin={
            .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(cmd,&begin));
        /* The layer rewrites PRESENT_SRC_KHR to TRANSFER_SRC_OPTIMAL on
         * virtual images ONLY. The app draws a different color per frame. */
        VkImageMemoryBarrier pre={
            .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=seen[index]?VK_ACCESS_TRANSFER_READ_BIT:0,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout=seen[index]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                                   VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=images[index],
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(cmd,seen[index]?
            VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&pre);
        const float level=0.15f+0.11f*(float)(frame%5u);
        const VkClearColorValue grey={.float32={level,level,level,1}};
        const VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &grey,1,&range);
        VkImageMemoryBarrier post=pre;
        post.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        post.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        post.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&post);
        TRY(vkEndCommandBuffer(cmd));
        TRY(vkResetFences(device,1,&rendering));
        const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        const VkSubmitInfo submit={
            .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount=1,.pWaitSemaphores=&image_ready,
            .pWaitDstStageMask=&stage,.commandBufferCount=1,
            .pCommandBuffers=&cmd,
            .signalSemaphoreCount=1,.pSignalSemaphores=&render_done};
        TRY(vkQueueSubmit(queue,1,&submit,rendering));
        VkResult single=VK_ERROR_UNKNOWN;
        const VkPresentInfoKHR present={
            .sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount=1,.pWaitSemaphores=&render_done,
            .swapchainCount=1,.pSwapchains=&virtual_handle,
            .pImageIndices=&index,.pResults=&single};
        VkResult presented=vkQueuePresentKHR(queue,&present);
        if(!gfg_test_wsi_result_usable(presented) ||
           !gfg_test_wsi_result_usable(single)) {
            fprintf(stderr,
                "GSF_DECK_WSI present_unusable=%d per_swapchain=%d frame=%u recreate_required=%d\n",
                (int)presented,(int)single,frame,
                gfg_test_wsi_recreate_required(presented) ||
                gfg_test_wsi_recreate_required(single));
            return 1;
        }
        if(presented==VK_SUBOPTIMAL_KHR || single==VK_SUBOPTIMAL_KHR)
            fprintf(stderr,"GSF_DECK_WSI present_suboptimal_usable=1 frame=%u\n",frame);
        const int expected=(int)(level*255.f+0.5f);
        /* X11 must expose the inserted A|B frame before the real B.
         * The synthetic GPU composite is unique: left pixel from A,
         * right pixel from B. A mere duplicate present cannot pass. */
        if(interleave && frame>1 && observe_pixels) {
            REQUIRE(last_level>=0 && last_level!=expected);
            if(hold_first && frame==2) {
                /* A was not displayed at its capture. The first owner
                 * output is A only AFTER B arrived, leaving real A,
                 * optional AB intermediates and B on one cadence grid. */
                uint64_t first_latency=0;
                REQUIRE(gfg_x11_wait_visible_pair(x,window,extent.width,
                    extent.height,last_level,last_level,1500000000ull,&first_latency));
                ++visible;
                ++output_visible;
                if(paced_source) {
                    x11_last_ns=gfg_pacer_monotonic_ns();
                    REQUIRE(x11_last_ns);
                }
                fprintf(stderr,"GSF_POST_OWNER_PRIME first_A_visible_after_B=1"
                    " source_causality_preserved=1 latency_us=%llu\n",
                    (unsigned long long)(first_latency/1000u));
            }
            for(unsigned middle=0;middle<middle_count;++middle) {
            const int blended=(last_level*(int)(lmc_multiplier-1-middle)+
                expected*(int)(middle+1)+(int)lmc_multiplier/2)/(int)lmc_multiplier;
            const int want_left=lmc_owner?blended:last_level;
            const int want_right=lmc_owner?blended:expected;
            uint64_t middle_latency=0;
            REQUIRE(gfg_x11_wait_visible_pair(x,window,extent.width,extent.height,
                    want_left,want_right,lmc_owner && frame<=3?20000000000ull:1500000000ull,&middle_latency));
            ++intermediate_visible;
            ++output_visible;
            if(paced_source) {
                const uint64_t observed=gfg_pacer_monotonic_ns();
                REQUIRE(observed);
                /* Warm-up A->first split naturally spans one SOURCE
                 * interval. All subsequent source/insert gaps are timed. */
                if(x11_last_ns && output_visible>=3) {
                    const uint64_t gap=observed-x11_last_ns;
                    x11_interval_total_ns+=gap;
                    if(gap<x11_interval_min_ns)x11_interval_min_ns=gap;
                    if(gap>x11_interval_max_ns)x11_interval_max_ns=gap;
                    ++x11_interval_count;
                }
                x11_last_ns=observed;
            }
            fprintf(stderr,"GSF_LOADER_SPLIT_APP frame=%u left=%d right=%d x11_visible=1 latency_us=%llu\n",
                    frame,want_left,want_right,(unsigned long long)(middle_latency/1000));
            if(lmc_owner)fprintf(stderr,"GSF_LMC_OWNER_PIXEL frame=%u output=%u tau=%.9f value=%d distinct_from_A=%d distinct_from_B=%d\n",
                frame,middle,(double)(middle+1)/lmc_multiplier,blended,blended!=last_level,blended!=expected);
            }
        }
        uint64_t latency=0;
        if(observe_pixels && !(hold_first && frame==1)) {
            REQUIRE(gfg_x11_wait_visible_pair(x,window,extent.width,extent.height,
                    expected,expected,lmc_owner && frame<=3?20000000000ull:1500000000ull,&latency));
        }
        if(!(hold_first && frame==1)) {
        ++visible;
        ++output_visible;
        }
        if(paced_source && observe_pixels && !(hold_first && frame==1)) {
            const uint64_t observed=gfg_pacer_monotonic_ns();
            REQUIRE(observed);
            if(x11_last_ns && output_visible>=3) {
                const uint64_t gap=observed-x11_last_ns;
                x11_interval_total_ns+=gap;
                if(gap<x11_interval_min_ns)x11_interval_min_ns=gap;
                if(gap>x11_interval_max_ns)x11_interval_max_ns=gap;
                ++x11_interval_count;
            }
            x11_last_ns=observed;
        }
        last_level=expected;
        seen[index]=1;
        if(gpu_only)
            fprintf(stderr,"GSF_LMC_OWNER_APP original_submit=%u api_accepted=1 x11_pixels=UNOBSERVED physical_scanout=UNVERIFIED\n",frame);
        else if(gamescope_observer)
            fprintf(stderr,
                "GSF_GAMESCOPE_APP original_submit=%u vk_present_accepted=1 x11_pixels=UNOBSERVABLE oled_scanout=UNVERIFIED\n",
                frame);
        else if(hold_first && frame==1)
            fprintf(stderr,"GSF_LOADER_LIVE_APP frame=1 virtual_present=1"
                           " first_real_pending=1 x11_visible=0\n");
        else
            fprintf(stderr,"GSF_LOADER_LIVE_APP frame=%u virtual_present=1 x11_visible=1 latency_us=%llu\\n",
                frame,(unsigned long long)(latency/1000));
    }
    REQUIRE(visible==frame_count);
    if(gpu_only) {
        fprintf(stderr,"GSF_LMC_OWNER_APP submitted=%u multiplier=%u expected_lmc_outputs=%u verification=WORKER_GPU_READBACK physical_scanout=UNVERIFIED\n",
            frame_count,lmc_multiplier,middle_count*(frame_count-1u));
    } else if(gamescope_observer) {
        /* We verified API submission only. Intermediate frames are scheduled
         * in the layer's worker, not sampled through the X11 backing store.
         * The outer script checks final worker present counts separately. */
        fprintf(stderr,
            "GSF_GAMESCOPE_SUBMIT source_originals=%u expected_diagnostic_intermediates=%u x11_readback=bypassed reason=GAMESCOPE_WSI oled_scanout=UNVERIFIED PASS\n",
            frame_count,interleave?frame_count-1u:0u);
    } else {
    REQUIRE(!interleave || intermediate_visible==middle_count*(frame_count-1u));
    REQUIRE(!interleave || output_visible==frame_count+middle_count*(frame_count-1u));
    if(interleave)
        fprintf(stderr,"GSF_LOADER_SPLIT_APP output_visible=%u original=%u intermediate=%u x11_distinct_pixels=1 PASS\n",
            output_visible,visible,intermediate_visible);
    if(paced_source) {
        REQUIRE(x11_interval_count==middle_count*(frame_count-1u)+frame_count-2u);
        const uint64_t mean_us=x11_interval_total_ns/x11_interval_count/1000u;
        const uint64_t min_us=x11_interval_min_ns/1000u;
        const uint64_t max_us=x11_interval_max_ns/1000u;
        fprintf(stderr,"GSF_PACED_%u_TO_%u_X11 visible=%u source_hz=%u target_hz=%u samples=%u mean_us=%llu min_us=%llu max_us=%llu\n",
            source_hz,output_hz,output_visible,source_hz,output_hz,
            x11_interval_count,(unsigned long long)mean_us,
            (unsigned long long)min_us,(unsigned long long)max_us);
        /* Software Xvfb, not physical 60Hz scanout. Use broad acceptance
         * for CI scheduling noise, but disallow the old 35ms intermediate
         * exposure and implausible zero-spacing duplicate Presents. */
        if(paced_45_90 || paced_225_90) {
            REQUIRE(mean_us>=5500 && mean_us<=20000);
            REQUIRE(min_us>=3000 && max_us<=35000);
        } else {
            REQUIRE(mean_us>=9000 && mean_us<=27000);
            REQUIRE(min_us>=5000 && max_us<=50000);
        }
        fprintf(stderr,"GSF_PACED_%u_TO_%u_X11 paced_source=%u originals=%u intermediate=%u visible=%u app_to_worker_distinct_frames=1 PASS\n",
                source_hz,output_hz,frame_count,visible,intermediate_visible,
                output_visible);
        if(paced_225_90)
            fprintf(stderr,"GSF_PACED_22P5_TO_90_X11 source_period_ns=%llu multiplier=4 originals=%u intermediate=%u visible=%u PASS\n",
                (unsigned long long)source_period_ns,visible,intermediate_visible,output_visible);
    }
    }
    TRY(vkDeviceWaitIdle(device));
    if(gamescope_observer) {
        /* Keep the actual Gamescope surface on screen for a short camera
         * observation period; do not alter pacing between video frames. */
        const struct timespec hold={.tv_sec=2,.tv_nsec=0};
        nanosleep(&hold,NULL);
        fprintf(stderr,"GSF_GAMESCOPE_CAPTURE_WINDOW hold_after_submit_ms=2000 oled_scanout=UNVERIFIED\n");
    }
    vkDestroyFence(device,rendering,NULL);
    vkDestroySemaphore(device,image_ready,NULL);
    vkDestroySemaphore(device,render_done,NULL);
    vkDestroyCommandPool(device,pool,NULL);
    vkDestroySwapchainKHR(device,virtual_handle,NULL);
    if(gpu_only)
        fprintf(stderr,"GSF_LMC_OWNER_APP_TERMINAL originals=%u api_submission=PASS physical_scanout=UNVERIFIED\n",frame_count);
    else if(gamescope_observer)
        fprintf(stderr,"GSF_GAMESCOPE_APP_TERMINAL originals=%u api_submission=PASS physical_scanout=UNVERIFIED\n",frame_count);
    else
        fprintf(stderr,"GSF_LOADER_LIVE_APP %u GPU-rendered real source frames visible through one output WSI PASS\\n",
            frame_count);

    vkDestroyDevice(device,NULL);
    vkDestroySurfaceKHR(instance,surface,NULL);
    vkDestroyInstance(instance,NULL);
    xcb_destroy_window(x,window);xcb_disconnect(x);
    if(gpu_only)
        puts("GSF LMC owner Vulkan submission completed; worker GPU pixels checked; physical scanout UNVERIFIED");
    else if(gamescope_observer)
        puts("GSF Gamescope API submission test completed; X11 pixel visibility and OLED scanout UNVERIFIED");
    else
        puts("GSF loader-installed virtual Acquire->Render->Present->X11 compositor PASS (not Deck)");
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real WSI smoke: two coloured *real* frames, XCB surface under Xvfb.
 * Exercises Vulkan acquire / barriers / submit / queue-present / retirement.
 * This does not claim TE generation or actual OLED scanout.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include <xcb/xcb.h>
#ifdef GFG_NATIVE_TEST_OWNED_WSI
#include "../src/output_owner.h"
#include "../src/virtual_wsi_facade.h"
#include "../src/output_proxy_bootstrap.h"
#include "owned_source_transport.h"
#include "../src/async_capture_retire.h"
#include "x11_output_probe.h"
#ifdef GFG_NATIVE_TEST_LAYER_PROXY_REAL
#define GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
#include "../src/layer.c"
#endif
#endif
#ifdef GFG_NATIVE_TEST_GPU_OBSERVER
#include "gpu_observer.h"
#endif

#ifdef GFG_NATIVE_TEST_WIDTH
#define TEST_WIDTH GFG_NATIVE_TEST_WIDTH
#define TEST_HEIGHT GFG_NATIVE_TEST_HEIGHT
#elif defined(GFG_NATIVE_TEST_SMALL)
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

#ifdef GFG_NATIVE_TEST_OWNED_WSI
/* Vulkan-backed callback bridge: the virtual source has API-shaped image
 * enumeration/acquire/present operations, while the ONLY physical WSI
 * swapchain remains the output XCB surface below. */
typedef struct {
    VkDevice device;
    VkQueue queue;
    VkCommandBuffer copy_cmd;
    VkFence copy_fence;
    gfg_source_probe *source;
    gfg_async_capture *async;
} gfg_vwsi_gpu_context;
static void vwsi_record_async_copy(void *user,VkCommandBuffer cmd,uint64_t ticket) {
    gfg_source_record_snapshot((gfg_source_probe *)user,cmd,ticket);
}
static VkResult vwsi_acquire_signal(void *arg,VkSemaphore signal,VkFence fence) {
    gfg_vwsi_gpu_context *ctx=arg;
    /* Vulkan binary semaphore acquire completion must be signalled on GPU.
     * Empty queue submit is allowed: do not host signal it. */
    const VkSubmitInfo submission={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount=signal?1u:0u,
        .pSignalSemaphores=signal?&signal:NULL};
    return vkQueueSubmit(ctx->queue,1,&submission,fence);
}
static VkResult vwsi_submit_capture(void *arg,uint32_t app_index,uint64_t token,
                  uint32_t wait_count,const VkSemaphore *waits) {
    gfg_vwsi_gpu_context *ctx=arg;
    if(!token||app_index!=token%ctx->source->app_image_count ||
       wait_count>16 || (wait_count && !waits))
        return VK_ERROR_INITIALIZATION_FAILED;
    if(ctx->async)
        return gfg_async_capture_submit(ctx->async,app_index,token,
                                        wait_count,waits);
    VkResult r=vkWaitForFences(ctx->device,1,&ctx->copy_fence,VK_TRUE,UINT64_MAX);
    if(r!=VK_SUCCESS)return r;
    r=vkResetCommandBuffer(ctx->copy_cmd,0);
    if(r!=VK_SUCCESS)return r;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    r=vkBeginCommandBuffer(ctx->copy_cmd,&begin);
    if(r!=VK_SUCCESS)return r;
    gfg_source_record_snapshot(ctx->source,ctx->copy_cmd,token);
    r=vkEndCommandBuffer(ctx->copy_cmd);
    if(r!=VK_SUCCESS)return r;
    r=vkResetFences(ctx->device,1,&ctx->copy_fence);
    if(r!=VK_SUCCESS)return r;
    VkPipelineStageFlags stages[16];
    for(uint32_t i=0;i<wait_count;++i)stages[i]=VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount=wait_count,.pWaitSemaphores=waits,
        .pWaitDstStageMask=wait_count?stages:NULL,
        .commandBufferCount=1,.pCommandBuffers=&ctx->copy_cmd};
    return vkQueueSubmit(ctx->queue,1,&submit,ctx->copy_fence);
}
#endif

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
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,.timelineSemaphore=VK_TRUE};
    di.pNext=&timeline;
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
        .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT
#if defined(GFG_NATIVE_TEST_GPU_OBSERVER) || defined(GFG_NATIVE_TEST_OWNED_WSI)
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
#endif
        ,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
        .preTransform=caps.currentTransform,.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    if (!(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        for(uint32_t bit=1;bit<=8;bit*=2) if(caps.supportedCompositeAlpha&bit)
            {swci.compositeAlpha=(VkCompositeAlphaFlagBitsKHR)bit;break;}
    }
    uint32_t total_frames=0;
    VkSwapchainKHR previous_swapchain=VK_NULL_HANDLE;
#ifdef GFG_NATIVE_TEST_RECREATE
    const uint32_t cycles=3;
#else
    const uint32_t cycles=1;
#endif
    for(uint32_t cycle=0;cycle<cycles;++cycle){
        if(cycle){
            uint32_t dimensions[]={TEST_WIDTH+16*cycle,TEST_HEIGHT+8*cycle};
            xcb_configure_window(xc,window,XCB_CONFIG_WINDOW_WIDTH|XCB_CONFIG_WINDOW_HEIGHT,dimensions);
            xcb_flush(xc);
            xcb_get_geometry_reply_t *geometry=xcb_get_geometry_reply(xc,xcb_get_geometry(xc,window),NULL);
            REQUIRE(geometry);free(geometry);
            TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));
            ext=caps.currentExtent;
            if(ext.width==UINT32_MAX) ext=(VkExtent2D){dimensions[0],dimensions[1]};
            swci.imageExtent=ext;
        }
        swci.oldSwapchain=previous_swapchain;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
#ifdef GFG_NATIVE_TEST_OWNED_WSI
#ifdef GFG_NATIVE_TEST_LAYER_PROXY_REAL
    const int real_layer_proxy=getenv("GFG_OWNER_TEST_LAYER_PROXY_REAL_DEVICE")!=NULL;
    native_device test_layer_device={0};
#else
    const int real_layer_proxy=0;
#endif
    const int real_proxy=real_layer_proxy ||
        getenv("GFG_OWNER_TEST_PROXY_REAL_DEVICE")!=NULL;
    /* A real driver now creates the only display WSI; the app's source
     * textures are independently allocated via Vulkan memory dispatch.
     * The virtual game handle NEVER reaches the driver. */
    gfg_output_proxy proxy={0};
    gfg_vwsi_gpu_context capture_ctx={0};
    gfg_offscreen_dispatch proxy_dispatch={
        .create_image=vkCreateImage,.destroy_image=vkDestroyImage,
        .get_image_requirements=vkGetImageMemoryRequirements,
        .get_memory_properties=vkGetPhysicalDeviceMemoryProperties,
        .allocate_memory=vkAllocateMemory,.free_memory=vkFreeMemory,
        .bind_image_memory=vkBindImageMemory
    };
    gfg_output_proxy *active_proxy=&proxy;
#ifdef GFG_NATIVE_TEST_LAYER_PROXY_REAL
    VkSwapchainKHR app_layer_handle=VK_NULL_HANDLE;
#endif
    if(real_proxy) {
        REQUIRE(cycles==1 && !previous_swapchain);
        const VkImageUsageFlags required=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
             VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        REQUIRE((caps.supportedUsageFlags & required)==required);
        VkSwapchainCreateInfoKHR app_ci=swci;
        app_ci.imageUsage |= required;
#ifdef GFG_NATIVE_TEST_LAYER_PROXY_REAL
        if(real_layer_proxy) {
            /* Drive the actual native layer GDPA-resolved vkCreateSwapchainKHR
             * against the live Vulkan device. Registration is injected by
             * this TEST binary, not installed in the global Vulkan loader. */
            native_device *d=&test_layer_device;
            d->handle=device;d->key=dispatch_key(device);d->physical=physical;
            d->create_swapchain=vkCreateSwapchainKHR;
            d->destroy_swapchain=vkDestroySwapchainKHR;
            d->get_images=vkGetSwapchainImagesKHR;
            d->acquire=vkAcquireNextImageKHR;d->present=vkQueuePresentKHR;
            d->test_proxy_ops=proxy_dispatch;
            d->test_proxy_signal=vwsi_acquire_signal;
            d->test_proxy_copy=vwsi_submit_capture;
            d->test_proxy_user=&capture_ctx;
            REQUIRE(!pthread_mutex_init(&d->lock,NULL));
            REQUIRE(!pthread_mutex_init(&d->queue_lock,NULL));
            REQUIRE(dispatch_key(device)==dispatch_key(queue));
            devices=d;
            REQUIRE(!setenv("GFG_NATIVE_TEST_PROXY_CREATE","1",1));
            PFN_vkCreateSwapchainKHR layer_create=(PFN_vkCreateSwapchainKHR)
                native_gdpa(device,"vkCreateSwapchainKHR");
            REQUIRE(layer_create);
            TRY(layer_create(device,&app_ci,NULL,&app_layer_handle));
            REQUIRE(d->swapchains && d->swapchains->test_proxy);
            active_proxy=d->swapchains->test_proxy;
            REQUIRE(app_layer_handle==gfg_output_proxy_app_handle(active_proxy));
        } else
#endif
        {
            TRY(gfg_output_proxy_create(&proxy,device,physical,&app_ci,
                vkCreateSwapchainKHR,vkDestroySwapchainKHR,&proxy_dispatch,
                &capture_ctx,vwsi_acquire_signal,vwsi_submit_capture));
        }
        swapchain=active_proxy->output;
        REQUIRE(gfg_output_proxy_app_handle(active_proxy)!=VK_NULL_HANDLE);
        fprintf(stderr,"GSF PROXY real Vulkan driver output WSI=1 app_images=%u owned_by_proxy=1 native_layer_create=%d\n",
            active_proxy->source.count,real_layer_proxy);
    } else {
        TRY(vkCreateSwapchainKHR(device,&swci,NULL,&swapchain));
    }
#else
    TRY(vkCreateSwapchainKHR(device,&swci,NULL,&swapchain));
#endif
    if(previous_swapchain) vkDestroySwapchainKHR(device,previous_swapchain,NULL);

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
    VkSemaphore acquired=VK_NULL_HANDLE;
    VkSemaphore *ready=calloc(image_count,sizeof(*ready));REQUIRE(ready);
    TRY(vkCreateSemaphore(device,&semci,NULL,&acquired));
    /* Render completion fences do not retire a WSI wait. Reuse a ready
     * semaphore only after reacquiring that same swapchain image. */
    for(uint32_t i=0;i<image_count;++i)TRY(vkCreateSemaphore(device,&semci,NULL,&ready[i]));
    unsigned char *seen=calloc(image_count,1);REQUIRE(seen);
#ifdef GFG_NATIVE_TEST_OWNED_WSI
    REQUIRE(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(ext.width>=8 && (ext.width%2)==0);
    gfg_source_probe source_probe;
    REQUIRE(gfg_source_probe_create(&source_probe,physical,device,ext,color.format));
    if(real_proxy) {
        REQUIRE(gfg_source_probe_adopt_app_pool(&source_probe,&active_proxy->source));
    } else {
        const char *app_images=getenv("GFG_OWNER_TEST_APP_IMAGES");
        if(app_images && !strcmp(app_images,"1"))source_probe.app_image_count=1;
    }
    /* Independent source renderer submits into OFFSCREEN VkImages.
     * They never reach vkQueuePresentKHR: the later output WSI owns it.
     * The separate fence protects source image write/overwrite lifetime. */
    VkCommandBuffer source_command=VK_NULL_HANDLE;
    VkCommandBuffer snapshot_command=VK_NULL_HANDLE;
    TRY(vkAllocateCommandBuffers(device,&cbai,&source_command));
    TRY(vkAllocateCommandBuffers(device,&cbai,&snapshot_command));
    VkFence source_fence=VK_NULL_HANDLE;
    VkFence snapshot_fence=VK_NULL_HANDLE;
    const VkFenceCreateInfo source_fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags=VK_FENCE_CREATE_SIGNALED_BIT};
    TRY(vkCreateFence(device,&source_fci,NULL,&source_fence));
    TRY(vkCreateFence(device,&source_fci,NULL,&snapshot_fence));
    /* The game GPU submission signals this binary semaphore; bridge-copy
     * submission waits on it. This is not an application CPU wait. */
    VkSemaphore source_ready=VK_NULL_HANDLE;
    const VkSemaphoreCreateInfo source_semci={
        .sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    TRY(vkCreateSemaphore(device,&source_semci,NULL,&source_ready));
    VkSemaphore app_image_acquired=VK_NULL_HANDLE;
    TRY(vkCreateSemaphore(device,&source_semci,NULL,&app_image_acquired));
    /* Virtual swapchain handle is NEVER passed to the loader or downstream
     * WSI: it belongs exclusively to the virtual app-source facade. */
    VkSwapchainKHR app_swapchain=real_proxy?
        gfg_output_proxy_app_handle(active_proxy):
        (VkSwapchainKHR)(uintptr_t)0x600FEEDu;
    capture_ctx=(gfg_vwsi_gpu_context){.device=device,.queue=queue,
        .copy_cmd=snapshot_command,.copy_fence=snapshot_fence,
        .source=&source_probe};
    gfg_virtual_wsi local_virtual_wsi={0};
    gfg_virtual_wsi *virtual_wsi=real_proxy?&active_proxy->app:&local_virtual_wsi;
    if(!real_proxy)REQUIRE(gfg_vwsi_init(virtual_wsi,app_swapchain,
        source_probe.game_frames,source_probe.app_image_count,color.format,
        ext,&capture_ctx,vwsi_acquire_signal,vwsi_submit_capture)==VK_SUCCESS);
    VkImage app_handles[GFG_VIRTUAL_MAX_IMAGES]={0};
    uint32_t app_image_count=GFG_VIRTUAL_MAX_IMAGES;
    TRY(gfg_vwsi_get_images(virtual_wsi,app_swapchain,&app_image_count,app_handles));
    REQUIRE(app_image_count==source_probe.app_image_count);
#ifdef GFG_NATIVE_TEST_LAYER_PROXY_REAL
    if(real_layer_proxy) {
        PFN_vkGetSwapchainImagesKHR layer_get=(PFN_vkGetSwapchainImagesKHR)
            native_gdpa(device,"vkGetSwapchainImagesKHR");
        REQUIRE(layer_get);
        uint32_t via_layer_count=0;
        TRY(layer_get(device,app_swapchain,&via_layer_count,NULL));
        REQUIRE(via_layer_count==app_image_count);
        VkImage via_layer[GFG_VIRTUAL_MAX_IMAGES]={0};
        TRY(layer_get(device,app_swapchain,&via_layer_count,via_layer));
        REQUIRE(via_layer_count==app_image_count);
        for(uint32_t i=0;i<via_layer_count;++i)
            REQUIRE(via_layer[i]==app_handles[i]);
    }
#endif
    gfg_async_capture async_capture={0};
    const int use_async_capture=getenv("GFG_OWNER_TEST_ASYNC_RETIRE")!=NULL;
    const int delayed_capture=getenv("GFG_OWNER_TEST_DELAYED_RETIRE")!=NULL;
    if(use_async_capture) {
        TRY(gfg_async_capture_init(&async_capture,virtual_wsi,device,queue,
                                   family,vwsi_record_async_copy,&source_probe));
        if(delayed_capture)async_capture.test_delayed_retire_ms=40u;
        capture_ctx.async=&async_capture;
    }
    uint32_t source_submissions=0,game_images_released=0,early_returns=0;
    uint32_t output_submitted_before_release=0;
#endif

    PFN_vkQueuePresentKHR present_gipa=(PFN_vkQueuePresentKHR)
        vkGetInstanceProcAddr(instance,"vkQueuePresentKHR");
    PFN_vkQueuePresentKHR present_gdpa=(PFN_vkQueuePresentKHR)
        vkGetDeviceProcAddr(device,"vkQueuePresentKHR");
    REQUIRE(present_gipa && present_gdpa);
#ifdef GFG_NATIVE_TEST_ASYNC
    VkFenceCreateInfo render_fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.flags=VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence render_fence;TRY(vkCreateFence(device,&render_fci,NULL,&render_fence));
#ifndef GFG_NATIVE_TEST_OWNED_WSI
    struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);
    const char *hz_env=getenv("GFG_NATIVE_TEST_SOURCE_HZ");
    unsigned source_hz=hz_env?(unsigned)strtoul(hz_env,NULL,10):30;
    REQUIRE(source_hz>=1 && source_hz<=240);
    const long source_period=1000000000l/source_hz;
    fprintf(stderr,"test source cadence_hz=%u clock=CLOCK_MONOTONIC catchup=disabled\n",source_hz);
#endif
    const char *count_env=getenv("GFG_NATIVE_TEST_FRAMES");
    uint32_t frame_count=count_env?(uint32_t)strtoul(count_env,NULL,10):300;
    REQUIRE(frame_count>0 && frame_count<=100000);
#else
    uint32_t frame_count=4;
#endif
#ifdef GFG_NATIVE_TEST_GPU_OBSERVER
    REQUIRE(observer_start(instance,physical,device,queue,swapchain,ext,color.format,
        images,image_count,family,present_gdpa));
#endif
#ifdef GFG_NATIVE_TEST_OWNED_WSI
    /* Emulate an independent game source feeding a PRIVATE compositor
     * output swapchain. The original real input is never sent to WSI from
     * a second path. This is WSI output transport smoke, not game capture. */
    const char *refresh_env=getenv("GFG_OWNER_TEST_OUTPUT_HZ");
    const char *source_env=getenv("GFG_OWNER_TEST_SOURCE_HZ");
    const char *gap_env=getenv("GFG_OWNER_TEST_GAP_AT");
    const char *drop_env=getenv("GFG_OWNER_TEST_GPU_DROP_EVERY");
    const unsigned refresh_hz=refresh_env?(unsigned)strtoul(refresh_env,NULL,10):60u;
    const unsigned source_hz=source_env?(unsigned)strtoul(source_env,NULL,10):30u;
    const unsigned gap_at=gap_env?(unsigned)strtoul(gap_env,NULL,10):0u;
    const unsigned drop_every=drop_env?(unsigned)strtoul(drop_env,NULL,10):0u;
    REQUIRE(refresh_hz>=24 && refresh_hz<=240);
    REQUIRE(source_hz>=1 && source_hz<=240);
    gfg_output_owner owner;
    gfg_owner_init(&owner,1000000000ull/refresh_hz,1000000000ull/20);
    const uint64_t input_step_ns=1000000000ull/source_hz;
    uint64_t capture_ns=1000000000ull;
    uint32_t output_calls=0, x11_observed=0;
    uint64_t x11_latency_sum_ns=0,x11_latency_max_ns=0;
    const int observe_x11=getenv("GFG_OWNER_TEST_X11_SCANOUT")!=NULL;
    uint64_t owner_first_due=0,owner_host_epoch=0;
    for(uint32_t source_id=1;source_id<=frame_count;++source_id) {
        unsigned acquired_app_image=UINT32_MAX;
#ifdef GFG_NATIVE_TEST_LAYER_PROXY_REAL
        if(real_layer_proxy) {
            PFN_vkAcquireNextImageKHR layer_acquire=(PFN_vkAcquireNextImageKHR)
                native_gdpa(device,"vkAcquireNextImageKHR");
            REQUIRE(layer_acquire);
            TRY(layer_acquire(device,app_swapchain,UINT64_MAX,
                app_image_acquired,VK_NULL_HANDLE,&acquired_app_image));
        } else
#endif
        TRY(gfg_vwsi_acquire(virtual_wsi,app_swapchain,UINT64_MAX,
                            app_image_acquired,VK_NULL_HANDLE,&acquired_app_image));
        REQUIRE(acquired_app_image==source_id%source_probe.app_image_count);
        REQUIRE(app_handles[acquired_app_image]==source_probe.game_frames[acquired_app_image]);
        /* Emulate a game presenting to a VIRTUAL offscreen swapchain:
         * submit its real image on GPU first, before any scheduler/WSI work.
         * The app frame's fence is retired before the source can be reused.
         * The same GPU queue is used to avoid unsupported cross-queue waits
         * in this first ownership prototype. */
        TRY(vkWaitForFences(device,1,&source_fence,VK_TRUE,UINT64_MAX));
        TRY(vkResetCommandBuffer(source_command,0));
        const VkCommandBufferBeginInfo source_begin={
            .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(source_command,&source_begin));
        gfg_source_record_game(&source_probe,source_command,source_id);
        TRY(vkEndCommandBuffer(source_command));
        TRY(vkResetFences(device,1,&source_fence));
        const VkPipelineStageFlags app_wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        const VkSubmitInfo source_submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount=1,.pWaitSemaphores=&app_image_acquired,
            .pWaitDstStageMask=&app_wait_stage,
            .commandBufferCount=1,.pCommandBuffers=&source_command,
            .signalSemaphoreCount=1,.pSignalSemaphores=&source_ready};
        TRY(vkQueueSubmit(queue,1,&source_submit,source_fence));
        /* App's vkQueuePresentKHR becomes an independent capture
         * submission. It consumes source_ready exactly once; no app
         * source image ever reaches the real output vkQueuePresentKHR. */
        uint64_t virtual_ticket=0;
        VkResult virtual_result=VK_ERROR_UNKNOWN;
        VkPresentInfoKHR source_present={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount=1,.pWaitSemaphores=&source_ready,
            .swapchainCount=1,.pSwapchains=&app_swapchain,
            .pImageIndices=&acquired_app_image,.pResults=&virtual_result};
#ifdef GFG_NATIVE_TEST_LAYER_PROXY_REAL
        if(real_layer_proxy) {
            PFN_vkQueuePresentKHR layer_present=(PFN_vkQueuePresentKHR)
                native_gdpa(device,"vkQueuePresentKHR");
            REQUIRE(layer_present);
            TRY(layer_present(queue,&source_present));
            virtual_ticket=virtual_wsi->source.token[acquired_app_image];
        } else
#endif
        TRY(gfg_vwsi_present(virtual_wsi,&source_present,&virtual_ticket));
        REQUIRE(virtual_result==VK_SUCCESS && virtual_ticket);
        if(use_async_capture) {
            /* Fault-injected worker retirement is delayed by 40ms after GPU
             * completion. The app's virtual Present must return while the
             * source image remains COPYING, before the worker frees it. */
            if(delayed_capture) {
                pthread_mutex_lock(&virtual_wsi->mutex);
                int still_copying=virtual_wsi->source.state[acquired_app_image] ==
                                  GFG_VIRTUAL_COPYING;
                pthread_mutex_unlock(&virtual_wsi->mutex);
                REQUIRE(still_copying);
                ++early_returns;
            }
            /* The GPU snapshot copy is queued before GPU output work on
             * the SAME VkQueue. Do not block on this fence before submitting
             * the output copy: command-buffer transfer barriers provide
             * visibility. Output readback still waits for its own fence. */
        } else {
            TRY(vkWaitForFences(device,1,&snapshot_fence,VK_TRUE,UINT64_MAX));
            TRY(gfg_vwsi_retire(virtual_wsi,acquired_app_image,virtual_ticket,1));
        }
        const unsigned history_slot=(unsigned)(source_id&1u);
        const unsigned app_slot=(unsigned)(source_id%source_probe.app_image_count);
        REQUIRE(source_probe.frames[history_slot]!=source_probe.game_frames[app_slot]);
        REQUIRE(source_probe.mem[history_slot]!=source_probe.game_mem[app_slot]);
        REQUIRE(source_probe.game_ids[app_slot]==source_id);
        REQUIRE(source_probe.ids[history_slot]==source_id);
        /* On async mode source reuse is NOT yet authorised here. Copy
         * retirement remains owned by the GPU worker, independently of
         * queueing the output images. */
        if(!use_async_capture)++game_images_released;
        ++source_submissions;
        fprintf(stderr,"GSF SOURCE render id=%u game_swapchain_virtual=1 history_copy_submitted=1 game_image_released=%d original_WSI_presents=0\n",
                source_id,!use_async_capture);
        gfg_owner_ticket owned[3];
        if(source_id==gap_at)capture_ns+=836000000ull;
        const uint64_t gpu_ready_ns=(drop_every && source_id%drop_every==0u)?
            capture_ns+250000000ull:capture_ns+4000000ull;
        const size_t planned=gfg_owner_capture(&owner,source_id,
            capture_ns,gpu_ready_ns,1,owned);
        capture_ns+=input_step_ns;
        for(size_t slot=0;slot<planned;++slot) {
        const uint32_t frame=output_calls++;
        const gfg_owner_ticket ticket=owned[slot];
        /* Pacing belongs exclusively to the output owner. Source frames
         * are not paced/presented by a second WSI loop. Map simulated due
         * timestamps to the actual CLOCK_MONOTONIC epoch once. */
        struct timespec clock_now;
        clock_gettime(CLOCK_MONOTONIC,&clock_now);
        if(!owner_first_due) {
            owner_first_due=ticket.due_ns;
            owner_host_epoch=(uint64_t)clock_now.tv_sec*1000000000ull+
                (uint64_t)clock_now.tv_nsec;
        }
        const uint64_t due=owner_host_epoch+
            (ticket.due_ns-owner_first_due);
        const struct timespec output_deadline={
            .tv_sec=(time_t)(due/1000000000ull),
            .tv_nsec=(long)(due%1000000000ull)};
        int sleep_rc;
        do { sleep_rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,
                                      &output_deadline,NULL); }
        while(sleep_rc==EINTR);
        REQUIRE(sleep_rc==0);
#else
    for(uint32_t frame=0;frame<frame_count;frame++) {
#endif
#ifdef GFG_NATIVE_TEST_ASYNC
        TRY(vkWaitForFences(device,1,&render_fence,VK_TRUE,UINT64_MAX));
#endif
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
#ifdef GFG_NATIVE_TEST_OWNED_WSI
        /* Real frames: GPU copy from captured OFFSCREEN source image.
         * Synthetic test frames: left and right halves of the two source
         * images, proving BOTH snapshots and no direct source WSI present. */
        REQUIRE(gfg_source_snapshot_ready(&source_probe,
                                           ticket.from_id,ticket.to_id));
        gfg_source_record_output(&source_probe,cmd,images[index],
            ticket.from_id,ticket.to_id,ticket.kind==GFG_OWNER_SYNTHETIC);
#else
        VkClearColorValue clear={.float32={0.20f+0.05f*(frame%4),0.2f,0.80f-0.05f*(frame%4),1}};
        if(getenv("GFG_NATIVE_TEST_DUPLICATE"))clear=(VkClearColorValue){.float32={0.25f,0.2f,0.75f,1}};
        if(getenv("GFG_NATIVE_TEST_CUT")) {
            float value=frame%2?.9f:.1f;
            clear=(VkClearColorValue){.float32={value,value,value,1}};
        }
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
        VkImageMemoryBarrier post=pre;
        post.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        post.dstAccessMask=0;
        post.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&post);
#endif
        TRY(vkEndCommandBuffer(cmd));
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount=1,.pWaitSemaphores=&acquired,.pWaitDstStageMask=&stage,
            .commandBufferCount=1,.pCommandBuffers=&cmd,
            .signalSemaphoreCount=1,.pSignalSemaphores=&ready[index]};
#ifdef GFG_NATIVE_TEST_ASYNC
        TRY(vkResetFences(device,1,&render_fence));
        TRY(vkQueueSubmit(queue,1,&si,render_fence));
#ifdef GFG_NATIVE_TEST_OWNED_WSI
        if(use_async_capture) {
            pthread_mutex_lock(&async_capture.lock);
            if(async_capture.retired<virtual_ticket)
                ++output_submitted_before_release;
            pthread_mutex_unlock(&async_capture.lock);
        }
#endif
#else
        TRY(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));
#endif
#ifdef GFG_NATIVE_TEST_OWNED_WSI
        /* Before WSI accepts an image, assert its GPU pixels reflect the
         * real A/B image IDs, not the number of submitted VkPresents.
         * This fence is intentionally test-only, not a release pacing loop. */
        TRY(vkWaitForFences(device,1,&render_fence,VK_TRUE,UINT64_MAX));
        REQUIRE(gfg_source_verify(&source_probe,ticket.from_id,ticket.to_id,
            ticket.kind==GFG_OWNER_SYNTHETIC));
#endif
        VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount=1,.pWaitSemaphores=&ready[index],
            .swapchainCount=1,.pSwapchains=&swapchain,.pImageIndices=&index};
#ifdef GFG_NATIVE_TEST_RESUME
        if(frame==1) REQUIRE(setenv("GFG_NATIVE_GPU","0",1)==0);
        if(frame==2) REQUIRE(setenv("GFG_NATIVE_GPU","1",1)==0);
#endif
#ifdef GFG_NATIVE_TEST_GPU_OBSERVER
        observer.frame=frame;observer.interval_calls=0;
        TRY(gfg_gpu_fg_present(observer.fg,queue,&pi,observer_present,NULL));
        const char *fault=getenv("GFG_NATIVE_GPU_FAULT");
        if(fault && !strcmp(fault,"record")) REQUIRE(observer.interval_calls==(frame==1?2u:1u));
#else
        if(frame%2==0) TRY(present_gipa(queue,&pi));
        else TRY(present_gdpa(queue,&pi));
#endif
#ifdef GFG_NATIVE_TEST_OWNED_WSI
        if(observe_x11) {
            const int expected_left=(int)(gfg_source_level(ticket.from_id)*255.0f+0.5f);
            const int expected_right=(int)(gfg_source_level(
                ticket.kind==GFG_OWNER_SYNTHETIC?ticket.to_id:ticket.from_id)*255.0f+0.5f);
            uint64_t visible_after_ns=0;
            REQUIRE(gfg_x11_wait_visible_pair(xc,window,ext.width,ext.height,
                expected_left,expected_right,250000000ull,&visible_after_ns));
            x11_latency_sum_ns+=visible_after_ns;
            if(visible_after_ns>x11_latency_max_ns)
                x11_latency_max_ns=visible_after_ns;
            ++x11_observed;
        }
        fprintf(stderr,"GSF OUTPUT WSI handoff=%u kind=%s pair=%llu:%llu due_ns=%llu acknowledged=1 physical_scanout=unverified\n",
            output_calls,ticket.kind==GFG_OWNER_REAL?"real":"intermediate",
            (unsigned long long)ticket.from_id,(unsigned long long)ticket.to_id,
            (unsigned long long)ticket.due_ns);
#endif
#ifdef GFG_NATIVE_TEST_ASYNC
#ifndef GFG_NATIVE_TEST_OWNED_WSI
        clock_gettime(CLOCK_MONOTONIC,&deadline);
        deadline.tv_nsec+=source_period;
        if(deadline.tv_nsec>=1000000000){++deadline.tv_sec;deadline.tv_nsec-=1000000000;}
        clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&deadline,NULL);
#endif
#else
        TRY(vkQueueWaitIdle(queue));
#endif
        seen[index]=1;
    }
#ifdef GFG_NATIVE_TEST_OWNED_WSI
        /* Output has already been submitted to the GPU (and test-verified).
         * Only now wait for the worker to retire the old source image before
         * advancing the source loop. Production must use a fully GPU-driven
         * output owner instead of this source-loop test dependency. */
        if(use_async_capture) {
            TRY(gfg_async_capture_wait_ticket(&async_capture,virtual_ticket));
            ++game_images_released;
        }
    }
    REQUIRE(owner.real_count==frame_count);
    REQUIRE(source_submissions==frame_count);
    REQUIRE(game_images_released==frame_count);
    REQUIRE(source_probe.virtual_present_new_rewrites==frame_count);
    REQUIRE(source_probe.virtual_present_old_rewrites==
        frame_count-source_probe.app_image_count);
    fprintf(stderr,"GSF virtual PRESENT layouts old=%llu new=%llu downstream_present_layout_on_offscreen=0 PASS\n",
        (unsigned long long)source_probe.virtual_present_old_rewrites,
        (unsigned long long)source_probe.virtual_present_new_rewrites);
    REQUIRE(virtual_wsi->source.acquired==frame_count);
    REQUIRE(virtual_wsi->source.retired==frame_count);
    REQUIRE(virtual_wsi->accepts==frame_count);
    REQUIRE(virtual_wsi->copy_retirements==frame_count);
    REQUIRE(gfg_virtual_all_idle(&virtual_wsi->source));
    REQUIRE(!virtual_wsi->source.invalid && !virtual_wsi->incorrect);
    if(use_async_capture) {
        REQUIRE(async_capture.submitted==frame_count);
        REQUIRE(async_capture.retired==frame_count);
        REQUIRE(async_capture.worker_gpu_waits==frame_count);
        REQUIRE(async_capture.app_gpu_waits==0);
        REQUIRE(async_capture.submit_failures==0);
        if(delayed_capture) {
            REQUIRE(early_returns==frame_count);
            REQUIRE(output_submitted_before_release>0);
            fprintf(stderr,"GSF slow-fence source stays COPYING after every virtual Present, deferred retirement=%u PASS\\n",
                early_returns);
            fprintf(stderr,"GSF GPU output submitted BEFORE worker source release=%u, same-queue order PASS\\n",
                output_submitted_before_release);
        }
        fprintf(stderr,"GSF async capture worker GPU fence waits=%llu presents=%llu app_thread_gpu_waits=%llu PASS\\n",
            (unsigned long long)async_capture.worker_gpu_waits,
            (unsigned long long)async_capture.submitted,
            (unsigned long long)async_capture.app_gpu_waits);
    }
    fprintf(stderr,"GSF virtual app source buffers=%u history_buffers=2 render_release_count=%u PASS\n",
        source_probe.app_image_count,game_images_released);
    REQUIRE(owner.synthetic_count==source_probe.verified_synthetic);
    REQUIRE(output_calls==owner.real_count+owner.synthetic_count);
    REQUIRE(owner.error_count==0);
    REQUIRE(!observe_x11 || x11_observed==output_calls);
    if(observe_x11) {
        fprintf(stderr,"GSF X11 compositor visible ordered frames=%u real=%llu intermediate=%llu PASS (not OLED)\n",
            x11_observed,(unsigned long long)owner.real_count,
            (unsigned long long)owner.synthetic_count);
        fprintf(stderr,"GSF X11 visibility after WSI: mean_us=%llu max_us=%llu samples=%u (not physical scanout)\n",
            (unsigned long long)(x11_latency_sum_ns/x11_observed/1000u),
            (unsigned long long)(x11_latency_max_ns/1000u),x11_observed);
    }
    REQUIRE(source_probe.verified_real==frame_count);
    REQUIRE(source_probe.verified_synthetic==owner.synthetic_count);
    REQUIRE((gap_at==0 || gap_at>frame_count) || owner.gap_count==1);
    fprintf(stderr,"GSF GPU source snapshots copied and readback verified: real=%u A/B_composites=%u PASS\\n",
        source_probe.verified_real,source_probe.verified_synthetic);
    if(use_async_capture)gfg_async_capture_cleanup(&async_capture);
    if(!real_proxy) {
        gfg_vwsi_close(virtual_wsi);
        gfg_vwsi_fini(virtual_wsi);
    }
    gfg_source_probe_destroy(&source_probe);
    vkDestroyFence(device,source_fence,NULL);
    vkDestroyFence(device,snapshot_fence,NULL);
    vkDestroySemaphore(device,source_ready,NULL);
    vkDestroySemaphore(device,app_image_acquired,NULL);
    fprintf(stderr,
        "GSF private-output WSI: real=%llu synthetic=%llu total=%u one_surface=1 API_ORDER=PASS physical_scanout=unverified\n",
        (unsigned long long)owner.real_count,
        (unsigned long long)owner.synthetic_count,output_calls);
#endif
    free(seen);
#ifdef GFG_NATIVE_TEST_ASYNC
    TRY(vkQueueWaitIdle(queue)); /* teardown only */
    vkDestroyFence(device,render_fence,NULL);
#endif
#ifdef GFG_NATIVE_TEST_GPU_OBSERVER
    REQUIRE(observer_finish());
#endif
    for(uint32_t i=0;i<image_count;++i)vkDestroySemaphore(device,ready[i],NULL);
    free(ready);
    vkDestroySemaphore(device,acquired,NULL);
    vkDestroyCommandPool(device,pool,NULL);
    free(images);
#ifdef GFG_NATIVE_TEST_OWNED_WSI
    if(real_proxy) {
        /* Present WSI has finished consuming the final app output and
         * snapshot history is already retired. Teardown is the only idle. */
        TRY(vkQueueWaitIdle(queue));
#ifdef GFG_NATIVE_TEST_LAYER_PROXY_REAL
        if(real_layer_proxy) {
            PFN_vkDestroySwapchainKHR layer_destroy=(PFN_vkDestroySwapchainKHR)
                native_gdpa(device,"vkDestroySwapchainKHR");
            REQUIRE(layer_destroy);
            layer_destroy(device,app_layer_handle,NULL);
            REQUIRE(!test_layer_device.swapchains);
            devices=NULL;
            pthread_mutex_destroy(&test_layer_device.lock);
            pthread_mutex_destroy(&test_layer_device.queue_lock);
            unsetenv("GFG_NATIVE_TEST_PROXY_CREATE");
            fprintf(stderr,"GSF PROXY real-driver native layer Create/GetImages/Acquire/Present/Destroy PASS\n");
        } else
#endif
        {
            gfg_output_proxy_destroy(&proxy,vkDestroySwapchainKHR,&proxy_dispatch);
            REQUIRE(proxy.output==VK_NULL_HANDLE && !proxy.source.count);
        }
        fprintf(stderr,"GSF PROXY real driver separate app/out WSI teardown PASS\n");
        swapchain=VK_NULL_HANDLE;
    }
#endif
    previous_swapchain=swapchain;
    total_frames+=frame_count;
    }
    vkDestroySwapchainKHR(device,previous_swapchain,NULL);
    vkDestroyDevice(device,NULL);
    vkDestroySurfaceKHR(instance,surface,NULL);
    vkDestroyInstance(instance,NULL);
    xcb_destroy_window(xc,window);xcb_disconnect(xc);
    printf("GFG native: %u real WSI acquire/submit/present cycles PASS\n",total_frames);
    return 0;
}

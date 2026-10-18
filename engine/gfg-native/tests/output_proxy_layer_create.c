/* SPDX-License-Identifier: GPL-3.0-or-later
 * TEST-ONLY: call the REAL layer's vkCreateSwapchainKHR resolver, then
 * validate a separate downstream output swapchain and offscreen app images.
 * All Vulkan implementations below are deterministic mocks. This is not
 * a third-party game, live driver, SteamOS, or OLED scanout acceptance.
 */
#define _GNU_SOURCE
#define GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS
#include "../src/layer.c"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static struct {
    unsigned outputs_created,outputs_destroyed,images_created,images_destroyed;
    unsigned memory_allocated,memory_freed,binds,signals,copies,presents;
    int fail_memory_at;
    VkImageUsageFlags last_output_usage;
    VkImageUsageFlags last_app_usage;
    const void *last_output_pnext;
} stats;

static VkDevice fake_device(void) {
    static void *key=(void *)(uintptr_t)0x9A00;
    return (VkDevice)&key;
}
static VkPhysicalDevice fake_physical(void) {
    return (VkPhysicalDevice)(uintptr_t)0x9200;
}
static VkSwapchainKHR make_swapchain(unsigned n) {
    return (VkSwapchainKHR)(uintptr_t)(0xD000u+n);
}
static VKAPI_ATTR VkResult VKAPI_CALL
mock_output_create(VkDevice device,const VkSwapchainCreateInfoKHR *ci,
                   const VkAllocationCallbacks *a,VkSwapchainKHR *out) {
    (void)a;assert(device==fake_device() && ci && out);
    ++stats.outputs_created;
    stats.last_output_usage=ci->imageUsage;
    stats.last_output_pnext=ci->pNext;
    *out=make_swapchain(stats.outputs_created);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
mock_output_destroy(VkDevice device,VkSwapchainKHR sc,const VkAllocationCallbacks *a) {
    (void)a;assert(device==fake_device()&&sc);
    ++stats.outputs_destroyed;
}
static VKAPI_ATTR VkResult VKAPI_CALL
mock_output_images(VkDevice device,VkSwapchainKHR sc,uint32_t *count,VkImage *images) {
    (void)device;(void)sc;
    if(!images) {*count=2;return VK_SUCCESS;}
    if(*count<2)return VK_INCOMPLETE;
    images[0]=(VkImage)(uintptr_t)0x101;images[1]=(VkImage)(uintptr_t)0x102;
    *count=2;return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL
mock_image_create(VkDevice device,const VkImageCreateInfo *ci,
                  const VkAllocationCallbacks *a,VkImage *out) {
    (void)a;assert(device==fake_device() && ci && out);
    stats.last_app_usage=ci->usage;
    ++stats.images_created;
    *out=(VkImage)(uintptr_t)(0xF000u+stats.images_created);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
mock_image_destroy(VkDevice device,VkImage image,const VkAllocationCallbacks *a) {
    (void)a;assert(device==fake_device() && image);
    ++stats.images_destroyed;
}
static VKAPI_ATTR void VKAPI_CALL
mock_requirements(VkDevice device,VkImage image,VkMemoryRequirements *r) {
    assert(device==fake_device() && image);
    *r=(VkMemoryRequirements){.size=4096,.alignment=256,.memoryTypeBits=1};
}
static VKAPI_ATTR void VKAPI_CALL
mock_memory_props(VkPhysicalDevice phys,VkPhysicalDeviceMemoryProperties *p) {
    assert(phys==fake_physical());
    memset(p,0,sizeof(*p));p->memoryTypeCount=1;
    p->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL
mock_allocate(VkDevice device,const VkMemoryAllocateInfo *a,
              const VkAllocationCallbacks *cb,VkDeviceMemory *memory) {
    (void)cb;assert(device==fake_device()&&a&&memory);
    ++stats.memory_allocated;
    if(stats.fail_memory_at &&
            (int)stats.memory_allocated==stats.fail_memory_at)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *memory=(VkDeviceMemory)(uintptr_t)(0xAA00u+stats.memory_allocated);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
mock_free(VkDevice device,VkDeviceMemory mem,const VkAllocationCallbacks *cb) {
    (void)cb;assert(device==fake_device()&&mem);
    ++stats.memory_freed;
}
static VKAPI_ATTR VkResult VKAPI_CALL
mock_bind(VkDevice device,VkImage image,VkDeviceMemory mem,VkDeviceSize offset) {
    assert(device==fake_device()&&image&&mem&&offset==0);
    ++stats.binds;return VK_SUCCESS;
}
static VkResult mock_signal(void *user,VkSemaphore sem,VkFence fence) {
    (void)user;assert(sem||fence);++stats.signals;return VK_SUCCESS;
}
static VkResult mock_capture(void *user,uint32_t image,uint64_t ticket,
                            uint32_t waits,const VkSemaphore *sem) {
    (void)user;(void)image;
    assert(ticket&&waits==1&&sem&&sem[0]);
    ++stats.copies;return VK_SUCCESS;
}
static VkSwapchainCreateInfoKHR app_request(void) {
    VkSwapchainCreateInfoKHR ci={0};
    ci.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.minImageCount=3;
    ci.imageFormat=VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent=(VkExtent2D){1280,800};
    ci.imageArrayLayers=1;
    ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;
    ci.surface=(VkSurfaceKHR)(uintptr_t)0xC11;
    return ci;
}
int main(void) {
    native_device d={0};d.handle=fake_device();
    d.key=dispatch_key(d.handle);d.physical=fake_physical();
    d.create_swapchain=mock_output_create;
    d.destroy_swapchain=mock_output_destroy;
    d.get_images=mock_output_images;
    d.test_proxy_ops=(gfg_offscreen_dispatch){
        .create_image=mock_image_create,.destroy_image=mock_image_destroy,
        .get_image_requirements=mock_requirements,
        .get_memory_properties=mock_memory_props,
        .allocate_memory=mock_allocate,.free_memory=mock_free,
        .bind_image_memory=mock_bind
    };
    d.test_proxy_signal=mock_signal;
    d.test_proxy_copy=mock_capture;
    assert(!pthread_mutex_init(&d.lock,NULL));
    assert(!pthread_mutex_init(&d.queue_lock,NULL));
    devices=&d;
    assert(!setenv("GFG_NATIVE_TEST_PROXY_CREATE","1",1));

    /* Device dispatch advertises only entrypoints actually supported by
     * the lower layer. Install test stubs BEFORE resolving API functions. */
    d.acquire=(PFN_vkAcquireNextImageKHR)(uintptr_t)1;
    d.present=(PFN_vkQueuePresentKHR)(uintptr_t)1;
    /* Obtain the API from the actual layer resolver, NOT by calling helper
     * directly. It creates one real output + three separate source images. */
    PFN_vkCreateSwapchainKHR create=(PFN_vkCreateSwapchainKHR)
        native_gdpa(d.handle,"vkCreateSwapchainKHR");
    PFN_vkDestroySwapchainKHR destroy=(PFN_vkDestroySwapchainKHR)
        native_gdpa(d.handle,"vkDestroySwapchainKHR");
    PFN_vkGetSwapchainImagesKHR get_images=(PFN_vkGetSwapchainImagesKHR)
        native_gdpa(d.handle,"vkGetSwapchainImagesKHR");
    PFN_vkAcquireNextImageKHR acquire=(PFN_vkAcquireNextImageKHR)
        native_gdpa(d.handle,"vkAcquireNextImageKHR");
    PFN_vkQueuePresentKHR present=(PFN_vkQueuePresentKHR)
        native_gdpa(d.handle,"vkQueuePresentKHR");
    assert(create&&destroy&&get_images&&acquire&&present);
    VkSwapchainCreateInfoKHR ci=app_request();
    VkSwapchainKHR virtual_handle=VK_NULL_HANDLE;
    assert(create(d.handle,&ci,NULL,&virtual_handle)==VK_SUCCESS);
    assert(virtual_handle && virtual_handle!=make_swapchain(1));
    assert(stats.outputs_created==1 && stats.images_created==3);
    assert(stats.memory_allocated==3 && stats.binds==3);
    assert(stats.last_output_usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    assert(stats.last_app_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    assert(ci.imageUsage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    uint32_t count=0;
    assert(get_images(d.handle,virtual_handle,&count,NULL)==VK_SUCCESS&&count==3);
    VkImage images[3]={0};count=3;
    assert(get_images(d.handle,virtual_handle,&count,images)==VK_SUCCESS);
    for(unsigned i=0;i<3;++i)
        assert(images[i] && images[i]!=images[(i+1)%3]);
    uint32_t idx=999;VkSemaphore acquired=(VkSemaphore)(uintptr_t)0x35;
    assert(acquire(d.handle,virtual_handle,0,acquired,VK_NULL_HANDLE,&idx)==VK_SUCCESS);
    assert(idx<3&&stats.signals==1);
    VkSemaphore finished=(VkSemaphore)(uintptr_t)0x36;
    VkResult per_swapchain=VK_ERROR_UNKNOWN;
    VkPresentInfoKHR info={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount=1,.pWaitSemaphores=&finished,
        .swapchainCount=1,.pSwapchains=&virtual_handle,
        .pImageIndices=&idx,.pResults=&per_swapchain};
    assert(present((VkQueue)d.handle,&info)==VK_SUCCESS);
    assert(per_swapchain==VK_SUCCESS && stats.copies==1);
    gfg_virtual_wsi *v=native_test_find_virtual(&d,virtual_handle);
    assert(v && v->source.state[idx]==GFG_VIRTUAL_COPYING);
    assert(gfg_vwsi_retire(v,idx,v->source.token[idx],1)==VK_SUCCESS);

    /* Invalid virtual oldSwapchain must not leak to downstream Vulkan. */
    ci.oldSwapchain=virtual_handle;
    VkSwapchainKHR next=VK_NULL_HANDLE;
    assert(create(d.handle,&ci,NULL,&next)==VK_ERROR_OUT_OF_DATE_KHR);
    assert(stats.outputs_created==1);ci.oldSwapchain=VK_NULL_HANDLE;

    destroy(d.handle,virtual_handle,NULL);
    assert(stats.outputs_destroyed==1&&stats.images_destroyed==3);
    assert(stats.memory_freed==3);
    assert(d.swapchains==NULL);
    puts("Layer vkCreateSwapchainKHR: one private output and three app images PASS");

    /* Any unrecognised app metadata uses original downstream create,
     * never an invalid virtual handle or partial offscreen state. */
    VkBaseInStructure unknown={.sType=(VkStructureType)1999912345};
    ci.pNext=&unknown;
    VkSwapchainKHR fallback=VK_NULL_HANDLE;
    assert(create(d.handle,&ci,NULL,&fallback)==VK_SUCCESS);
    assert(fallback==make_swapchain(2)&&stats.last_output_pnext==&unknown);
    assert(stats.outputs_created==2&&stats.images_created==3);
    destroy(d.handle,fallback,NULL);
    assert(stats.outputs_destroyed==2);
    ci.pNext=NULL;

    /* If GPU memory allocation fails after creation of the real output,
     * destroy that output AND every partial offscreen resource, then safely
     * retry the unchanged app WSI. */
    stats.fail_memory_at=(int)stats.memory_allocated+2;
    assert(create(d.handle,&ci,NULL,&fallback)==VK_SUCCESS);
    assert(fallback==make_swapchain(4));
    assert(stats.outputs_created==4&&stats.outputs_destroyed==3);
    assert(stats.images_created==5&&stats.images_destroyed==5);
    assert(stats.memory_allocated==5&&stats.memory_freed==4);
    destroy(d.handle,fallback,NULL);
    assert(stats.outputs_destroyed==4&&d.swapchains==NULL);
    devices=NULL;
    pthread_mutex_destroy(&d.lock);
    pthread_mutex_destroy(&d.queue_lock);
    unsetenv("GFG_NATIVE_TEST_PROXY_CREATE");
    puts("Layer vkCreateSwapchainKHR: unknown pNext and OOM rollback PASS");
    puts("TEST-ONLY MOCKED Vulkan; no actual game or Deck scanout proof");
    return 0;
}

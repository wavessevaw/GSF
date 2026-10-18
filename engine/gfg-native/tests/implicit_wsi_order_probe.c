/* SPDX-License-Identifier: GPL-3.0-or-later
 * CI-only implicit WSI ordering probe. NOT Gamescope, NOT shipped in the kit.
 * Rewrites the real driver's swapchain/present modes to MAILBOX, as Gamescope
 * does, but without Wayland protocol, maintenance1 emulation or fake images.
 * Logs whether it saw app-only or GSF-extended usage/image count. Real Mesa
 * owns every VkSwapchain/VkImage, and actual GPU LMC runs above this layer.
 */
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define EXPORT __attribute__((visibility("default")))
static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkCreateSwapchainKHR next_create;
static PFN_vkQueuePresentKHR next_present;
static atomic_uint presents;
static VkSwapchainKHR tracked;
static VkPresentModeKHR driver_mode;
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL probe_gipa(VkInstance,const char *);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL probe_gdpa(VkDevice,const char *);

static VKAPI_ATTR VkResult VKAPI_CALL probe_instance(const VkInstanceCreateInfo *ci,
        const VkAllocationCallbacks *alloc,VkInstance *out) {
    VkLayerInstanceCreateInfo *link=(void *)ci->pNext;
    while(link && !(link->sType==VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                   link->function==VK_LAYER_LINK_INFO))link=(void *)link->pNext;
    if(!link || !link->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
    next_gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance create=(void *)next_gipa(VK_NULL_HANDLE,"vkCreateInstance");
    link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    return create(ci,alloc,out);
}
static VKAPI_ATTR VkResult VKAPI_CALL probe_device(VkPhysicalDevice phys,
        const VkDeviceCreateInfo *ci,const VkAllocationCallbacks *alloc,VkDevice *out) {
    VkLayerDeviceCreateInfo *link=(void *)ci->pNext;
    while(link && !(link->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                   link->function==VK_LAYER_LINK_INFO))link=(void *)link->pNext;
    if(!link || !link->u.pLayerInfo)return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gdpa=link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice create=(void *)gipa(VK_NULL_HANDLE,"vkCreateDevice");
    link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    VkResult r=create(phys,ci,alloc,out);
    if(r==VK_SUCCESS) {
        next_create=(void *)next_gdpa(*out,"vkCreateSwapchainKHR");
        next_present=(void *)next_gdpa(*out,"vkQueuePresentKHR");
    }
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL probe_create(VkDevice d,
        const VkSwapchainCreateInfoKHR *ci,const VkAllocationCallbacks *alloc,VkSwapchainKHR *out) {
    fprintf(stderr,"GSF_ORDER_PROBE_CREATE input_mode=%d images=%u usage=0x%x pnext=%d\n",
        ci->presentMode,ci->minImageCount,ci->imageUsage,
        ci->pNext?(int)((const VkBaseInStructure *)ci->pNext)->sType:-1);
    VkSwapchainCreateInfoKHR driver=*ci;
    driver.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
    /* The probe's live path uses no maintenance1 create list. The exact
     * MAILBOX-only field chain is separately checked by swapchain-policy. */
    if(ci->pNext)return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult r=next_create(d,&driver,alloc,out);
    if(r==VK_SUCCESS){tracked=*out;driver_mode=driver.presentMode;}
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL probe_present(VkQueue q,const VkPresentInfoKHR *pi) {
    VkPresentInfoKHR driver=*pi;
    if(pi->swapchainCount!=1 || pi->pSwapchains[0]!=tracked)return VK_ERROR_INITIALIZATION_FAILED;
    const unsigned n=atomic_fetch_add(&presents,1)+1;
    fprintf(stderr,"GSF_ORDER_PROBE_PRESENT n=%u requested_driver_mode=%d\n",n,driver_mode);
    return next_present(q,&driver);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL probe_gdpa(VkDevice d,const char *n) {
    if(!strcmp(n,"vkGetDeviceProcAddr"))return (void *)probe_gdpa;
    if(!strcmp(n,"vkCreateSwapchainKHR"))return (void *)probe_create;
    if(!strcmp(n,"vkQueuePresentKHR"))return (void *)probe_present;
    return next_gdpa?next_gdpa(d,n):NULL;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL probe_gipa(VkInstance i,const char *n) {
    if(!strcmp(n,"vkGetInstanceProcAddr"))return (void *)probe_gipa;
    if(!strcmp(n,"vkGetDeviceProcAddr"))return (void *)probe_gdpa;
    if(!strcmp(n,"vkCreateInstance"))return (void *)probe_instance;
    if(!strcmp(n,"vkCreateDevice"))return (void *)probe_device;
    if(!strcmp(n,"vkCreateSwapchainKHR"))return (void *)probe_create;
    if(!strcmp(n,"vkQueuePresentKHR"))return (void *)probe_present;
    return next_gipa?next_gipa(i,n):NULL;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL probe_phys(VkInstance i,const char *n) {
    return next_gipa?next_gipa(i,n):NULL;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
        VkNegotiateLayerInterface *v) {
    if(v->loaderLayerInterfaceVersion>2)v->loaderLayerInterfaceVersion=2;
    v->pfnGetInstanceProcAddr=probe_gipa;v->pfnGetDeviceProcAddr=probe_gdpa;
    v->pfnGetPhysicalDeviceProcAddr=probe_phys;
    return VK_SUCCESS;
}

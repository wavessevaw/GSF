/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only lower-layer physical handle with NO loader dispatch table.
 * Only the matching next-layer callbacks may consume this handle. GPU/device
 * operations still use the real Vulkan device. Not a Gamescope emulation.
 */
#ifndef GSF_TEST_WRAPPED_PHYSICAL_H
#define GSF_TEST_WRAPPED_PHYSICAL_H
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
static VkPhysicalDevice gsf_test_real_physical;
static uintptr_t gsf_test_physical_token[2];
static unsigned gsf_test_format_queries,gsf_test_packed_queries;
static VkPhysicalDevice gsf_test_unwrap(VkPhysicalDevice p) {
    assert(p==(VkPhysicalDevice)gsf_test_physical_token);
    return gsf_test_real_physical;
}
static VKAPI_ATTR void VKAPI_CALL gsf_test_families(VkPhysicalDevice p,uint32_t *n,VkQueueFamilyProperties *v) {
    vkGetPhysicalDeviceQueueFamilyProperties(gsf_test_unwrap(p),n,v);
}
static VKAPI_ATTR void VKAPI_CALL gsf_test_formats(VkPhysicalDevice p,VkFormat f,VkFormatProperties *v) {
    ++gsf_test_format_queries;
    if(f==VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f==VK_FORMAT_A2R10G10B10_UNORM_PACK32)++gsf_test_packed_queries;
    vkGetPhysicalDeviceFormatProperties(gsf_test_unwrap(p),f,v);
}
static VKAPI_ATTR void VKAPI_CALL gsf_test_properties(VkPhysicalDevice p,VkPhysicalDeviceProperties *v) {
    vkGetPhysicalDeviceProperties(gsf_test_unwrap(p),v);
}
static VKAPI_ATTR void VKAPI_CALL gsf_test_properties2(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *v) {
    vkGetPhysicalDeviceProperties2(gsf_test_unwrap(p),v);
}
static VKAPI_ATTR void VKAPI_CALL gsf_test_memory(VkPhysicalDevice p,VkPhysicalDeviceMemoryProperties *v) {
    vkGetPhysicalDeviceMemoryProperties(gsf_test_unwrap(p),v);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gsf_test_instance_dispatch(VkInstance i,const char *n) {
#define PHYS(name,fn) if(!strcmp(n,name))return (PFN_vkVoidFunction)fn;
    PHYS("vkGetPhysicalDeviceQueueFamilyProperties",gsf_test_families)
    PHYS("vkGetPhysicalDeviceFormatProperties",gsf_test_formats)
    PHYS("vkGetPhysicalDeviceProperties",gsf_test_properties)
    PHYS("vkGetPhysicalDeviceProperties2",gsf_test_properties2)
    PHYS("vkGetPhysicalDeviceProperties2KHR",gsf_test_properties2)
    PHYS("vkGetPhysicalDeviceMemoryProperties",gsf_test_memory)
#undef PHYS
    return vkGetInstanceProcAddr(i,n);
}
static int gsf_test_wrap_physical(VkPhysicalDevice real,VkPhysicalDevice *borrowed,PFN_vkGetInstanceProcAddr *gipa) {
    if(!getenv("GFG_TEST_WRAPPED_PHYSICAL"))return 0;
    gsf_test_real_physical=real;*borrowed=(VkPhysicalDevice)gsf_test_physical_token;
    *gipa=gsf_test_instance_dispatch;return 1;
}
static void gsf_test_wrapped_result(int enabled,int require_packed) {
    if(!enabled)return;
    assert(gsf_test_format_queries>=4);
    if(require_packed)assert(gsf_test_packed_queries>0);
    fprintf(stderr,"WRAPPED_PHYSICAL_DISPATCH_PASS format_queries=%u packed_queries=%u loader_handle_compatible=0 Gamescope_test=0\n",
        gsf_test_format_queries,gsf_test_packed_queries);
}
#endif

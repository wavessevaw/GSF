/* SPDX-License-Identifier: GPL-3.0-or-later
 * Mock-Vulkan failure injection for actual VkImage/VkDeviceMemory ownership.
 * Not a real application, compositor, or Deck OLED visibility test.
 */
#include "../src/offscreen_source_pool.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    int creates, destroys, allocations, frees, binds;
    int fail_create_at, fail_allocate_at, fail_bind_at;
    uint32_t memory_type_bits;
    VkImageUsageFlags recorded_usage;
    uint32_t recorded_memory_type;
} fake_state;
static fake_state state;

static VkImage fake_image(uintptr_t id) {
    return (VkImage)(uintptr_t)(id + 0x1000);
}
static VkDeviceMemory fake_memory(uintptr_t id) {
    return (VkDeviceMemory)(uintptr_t)(id + 0x2000);
}
static VkDevice fake_device(void) {
    return (VkDevice)(uintptr_t)0x3000;
}
static VkPhysicalDevice fake_physical(void) {
    return (VkPhysicalDevice)(uintptr_t)0x4000;
}
static VKAPI_ATTR VkResult VKAPI_CALL
fake_create_image(VkDevice dev, const VkImageCreateInfo *ci,
        const VkAllocationCallbacks *callbacks, VkImage *out) {
    (void)callbacks;
    assert(dev == fake_device() && out);
    assert(ci->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    assert(ci->imageType == VK_IMAGE_TYPE_2D);
    assert(ci->extent.width == 1280 && ci->extent.height == 800 &&
           ci->extent.depth == 1 && ci->arrayLayers == 1);
    assert(ci->samples == VK_SAMPLE_COUNT_1_BIT);
    assert(ci->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    assert(ci->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    state.recorded_usage = ci->usage;
    ++state.creates;
    if (state.fail_create_at == state.creates)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = fake_image((uintptr_t)state.creates);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
fake_destroy_image(VkDevice dev, VkImage image,
        const VkAllocationCallbacks *callbacks) {
    (void)callbacks;
    assert(dev == fake_device() && image);
    ++state.destroys;
}
static VKAPI_ATTR void VKAPI_CALL
fake_requirements(VkDevice dev, VkImage image, VkMemoryRequirements *req) {
    assert(dev == fake_device() && image && req);
    *req = (VkMemoryRequirements){
        .size = 8192, .alignment = 256,
        .memoryTypeBits = state.memory_type_bits
    };
}
static VKAPI_ATTR void VKAPI_CALL
fake_memory_properties(VkPhysicalDevice physical,
        VkPhysicalDeviceMemoryProperties *mp) {
    assert(physical == fake_physical() && mp);
    memset(mp, 0, sizeof(*mp));
    mp->memoryTypeCount = 2;
    mp->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    mp->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL
fake_allocate(VkDevice dev, const VkMemoryAllocateInfo *ci,
        const VkAllocationCallbacks *callbacks, VkDeviceMemory *out) {
    (void)callbacks;
    assert(dev == fake_device() && ci && out);
    assert(ci->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    assert(ci->allocationSize == 8192);
    state.recorded_memory_type = ci->memoryTypeIndex;
    ++state.allocations;
    if (state.fail_allocate_at == state.allocations)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = fake_memory((uintptr_t)state.allocations);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
fake_free(VkDevice dev, VkDeviceMemory memory,
        const VkAllocationCallbacks *callbacks) {
    (void)callbacks;
    assert(dev == fake_device() && memory);
    ++state.frees;
}
static VKAPI_ATTR VkResult VKAPI_CALL
fake_bind(VkDevice dev, VkImage image, VkDeviceMemory memory,
        VkDeviceSize offset) {
    assert(dev == fake_device() && image && memory && offset == 0);
    ++state.binds;
    if (state.fail_bind_at == state.binds)
        return VK_ERROR_DEVICE_LOST;
    return VK_SUCCESS;
}
static const gfg_offscreen_dispatch dispatch = {
    .create_image = fake_create_image,
    .destroy_image = fake_destroy_image,
    .get_image_requirements = fake_requirements,
    .get_memory_properties = fake_memory_properties,
    .allocate_memory = fake_allocate,
    .free_memory = fake_free,
    .bind_image_memory = fake_bind
};
static VkSwapchainCreateInfoKHR eligible(void) {
    VkSwapchainCreateInfoKHR ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.minImageCount = 3;
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = (VkExtent2D){1280,800};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    return ci;
}
static void reset(void) {
    memset(&state, 0, sizeof(state));
    state.memory_type_bits = 3u;
}
static void complete_path(void) {
    reset();
    VkSwapchainCreateInfoKHR ci = eligible();
    gfg_offscreen_source_pool pool = {0};
    assert(!gfg_offscreen_unsupported(&ci));
    assert(gfg_offscreen_pool_create(&pool,fake_device(),fake_physical(),
                                    &ci,&dispatch) == VK_SUCCESS);
    assert(pool.count == 3);
    assert(state.creates == 3 && state.allocations == 3 && state.binds == 3);
    assert(state.recorded_memory_type == 1);
    assert(state.recorded_usage == (ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    assert(pool.images[0] != pool.images[1] &&
           pool.images[1] != pool.images[2]);
    assert(gfg_offscreen_pool_create(&pool,fake_device(),fake_physical(),
                                    &ci,&dispatch) == VK_ERROR_INITIALIZATION_FAILED);
    gfg_offscreen_pool_destroy(&pool,fake_device(),&dispatch);
    assert(!pool.count && !pool.images[0] && !pool.memory[0]);
    assert(state.destroys == 3 && state.frees == 3);
}
static void rollback_paths(void) {
    VkSwapchainCreateInfoKHR ci = eligible();
    gfg_offscreen_source_pool pool = {0};
    reset(); state.fail_create_at = 2;
    assert(gfg_offscreen_pool_create(&pool,fake_device(),fake_physical(),
                                    &ci,&dispatch) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(state.creates == 2 && state.destroys == 1);
    assert(state.allocations == 1 && state.frees == 1 && !pool.count);
    reset(); state.fail_allocate_at = 2;
    assert(gfg_offscreen_pool_create(&pool,fake_device(),fake_physical(),
                                    &ci,&dispatch) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    assert(state.creates == 2 && state.destroys == 2);
    assert(state.allocations == 2 && state.frees == 1 && !pool.count);
    reset(); state.fail_bind_at = 2;
    assert(gfg_offscreen_pool_create(&pool,fake_device(),fake_physical(),
                                    &ci,&dispatch) == VK_ERROR_DEVICE_LOST);
    assert(state.creates == 2 && state.destroys == 2);
    assert(state.allocations == 2 && state.frees == 2 && !pool.count);
    reset(); state.memory_type_bits = 0;
    assert(gfg_offscreen_pool_create(&pool,fake_device(),fake_physical(),
                                    &ci,&dispatch) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(state.creates == 1 && state.destroys == 1);
    assert(state.allocations == 0 && state.frees == 0);
    reset(); state.memory_type_bits = 1u;
    assert(gfg_offscreen_pool_create(&pool,fake_device(),fake_physical(),
                                    &ci,&dispatch) == VK_SUCCESS);
    assert(state.recorded_memory_type == 0);
    gfg_offscreen_pool_destroy(&pool,fake_device(),&dispatch);
}
static void unknown_create_metadata_is_rejected(void) {
    reset();
    VkSwapchainCreateInfoKHR ci = eligible();
    gfg_offscreen_source_pool pool = {0};
    VkBaseInStructure extension = {.sType = (VkStructureType)1999912345};
    ci.pNext = &extension;
    assert(gfg_offscreen_unsupported(&ci));
    assert(gfg_offscreen_pool_create(&pool,fake_device(),fake_physical(),
                                    &ci,&dispatch) == VK_ERROR_FEATURE_NOT_PRESENT);
    ci.pNext = NULL;
    ci.oldSwapchain = (VkSwapchainKHR)(uintptr_t)0xBAD;
    assert(gfg_offscreen_unsupported(&ci)); ci.oldSwapchain = VK_NULL_HANDLE;
    ci.imageFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    assert(gfg_offscreen_unsupported(&ci));
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    assert(gfg_offscreen_unsupported(&ci));
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    assert(gfg_offscreen_unsupported(&ci));
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    assert(gfg_offscreen_unsupported(&ci));
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.flags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR;
    assert(gfg_offscreen_unsupported(&ci));
    assert(state.creates == 0 && state.allocations == 0);
}
int main(void) {
    complete_path();
    rollback_paths();
    unknown_create_metadata_is_rejected();
    puts("GSF offscreen source pool: GPU memory dispatch + atomic rollback PASS");
    puts("NOT a Vulkan layer create hook, real game, Gamescope, or OLED test");
    return 0;
}

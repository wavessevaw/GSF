/* SPDX-License-Identifier: GPL-3.0-or-later
 * Smoke the real Vulkan loader -> GFG native layer -> lavapipe dispatch chain.
 * No game presentation, display, or frame generation is claimed here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(call) do { VkResult rc = (call); if (rc != VK_SUCCESS) { \
    fprintf(stderr, "%s failed: %d\n", #call, rc); return 1; } } while (0)

int main(void) {
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "gfg-native-loader-smoke",
        .pEngineName = "gfg-native-test",
        .apiVersion = VK_API_VERSION_1_2
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app
    };
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&ci, NULL, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) {
        fprintf(stderr, "No Vulkan physical devices\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) { vkDestroyInstance(instance, NULL); return 1; }
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family_index = UINT32_MAX;
    for (uint32_t i = 0; i < count && !physical; ++i) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        VkQueueFamilyProperties *q = calloc(families, sizeof(*q));
        if (!q) break;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, q);
        for (uint32_t k = 0; k < families; ++k) {
            if (q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical = devices[i];
                family_index = k;
                break;
            }
        }
        free(q);
    }
    free(devices);
    if (!physical) {
        fprintf(stderr, "No graphics queue found\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    float priority = 1.f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family_index,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci
    };
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family_index, 0, &queue);
    if (!queue) {
        fprintf(stderr, "Missing device queue\n");
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    PFN_vkVoidFunction p = vkGetDeviceProcAddr(device, "vkQueueSubmit");
    if (!p) {
        fprintf(stderr, "vkGetDeviceProcAddr dispatch broken\n");
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    puts("GFG native: Vulkan loader/instance/device dispatch smoke PASS");
    return 0;
}

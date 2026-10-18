#ifndef GSF_TEST_VULKAN_STUB_H
#define GSF_TEST_VULKAN_STUB_H
#define VKAPI_PTR
#include <stdint.h>
typedef void *VkDevice;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance,const char*);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice,const char*);
typedef int VkTimeDomainKHR;
typedef struct {int sType;const void *pNext;VkTimeDomainKHR timeDomain;} VkCalibratedTimestampInfoKHR;
#define VK_INCOMPLETE 5
#define VK_ERROR_INITIALIZATION_FAILED -3
#define VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR 1000184000
#define VK_TIME_DOMAIN_DEVICE_KHR 0
#define VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR 1

typedef uint64_t VkSemaphore;
typedef int32_t VkResult;
#define VK_SUCCESS 0
#define VK_TIMEOUT 2
#define VK_ERROR_DEVICE_LOST -4
#define VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO 1000207004
typedef struct {int sType; const void *pNext; uint32_t flags; uint32_t semaphoreCount;
    const VkSemaphore *pSemaphores; const uint64_t *pValues;} VkSemaphoreWaitInfo;
typedef VkResult (*PFN_vkWaitSemaphores)(VkDevice,const VkSemaphoreWaitInfo*,uint64_t);
typedef VkResult (*PFN_vkGetSemaphoreCounterValue)(VkDevice,VkSemaphore,uint64_t*);
#endif

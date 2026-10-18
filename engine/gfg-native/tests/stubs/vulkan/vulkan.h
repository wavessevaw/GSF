#ifndef GSF_TEST_VULKAN_STUB_H
#define GSF_TEST_VULKAN_STUB_H
#include <stdint.h>
typedef void *VkDevice;
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

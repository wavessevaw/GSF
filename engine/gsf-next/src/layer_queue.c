/* Queue-level and device-level calls: the application's own on the shared queue, serialised
 * with the layer's, and the device's destruction. */

#include "layer_internal.h"


VkResult afmf_device_queue_present(struct afmf_device *dev, VkQueue queue,
                                   const VkPresentInfoKHR *info)
{
    pthread_mutex_t *mutex=afmf_device_queue_mutex(dev,queue);
    pthread_mutex_lock(mutex);
    VkResult res = dev->fns.queue_present(queue, info);
    pthread_mutex_unlock(mutex);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueSubmit(VkQueue queue, uint32_t count,
                                                const VkSubmitInfo *submits, VkFence fence)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(queue));
    if (dev == NULL)
        return VK_ERROR_DEVICE_LOST;
    pthread_mutex_t *mutex=afmf_device_queue_mutex(dev,queue);
    pthread_mutex_lock(mutex);
    VkResult res = dev->fns.queue_submit(queue, count, submits, fence);
    pthread_mutex_unlock(mutex);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueSubmit2(VkQueue queue, uint32_t count,
                                                 const VkSubmitInfo2 *submits, VkFence fence)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(queue));
    if (dev == NULL || dev->fns.queue_submit2 == NULL)
        return VK_ERROR_DEVICE_LOST;
    pthread_mutex_t *mutex=afmf_device_queue_mutex(dev,queue);
    pthread_mutex_lock(mutex);
    VkResult res = dev->fns.queue_submit2(queue, count, submits, fence);
    pthread_mutex_unlock(mutex);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueSubmit2KHR(VkQueue queue, uint32_t count,
                                                    const VkSubmitInfo2 *submits, VkFence fence)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(queue));
    if (dev == NULL || dev->fns.queue_submit2_khr == NULL)
        return VK_ERROR_DEVICE_LOST;
    pthread_mutex_t *mutex=afmf_device_queue_mutex(dev,queue);
    pthread_mutex_lock(mutex);
    VkResult res = dev->fns.queue_submit2_khr(queue, count, submits, fence);
    pthread_mutex_unlock(mutex);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueBindSparse(VkQueue queue, uint32_t count,
                                                    const VkBindSparseInfo *binds, VkFence fence)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(queue));
    if (dev == NULL || dev->fns.queue_bind_sparse == NULL)
        return VK_ERROR_DEVICE_LOST;
    pthread_mutex_t *mutex=afmf_device_queue_mutex(dev,queue);
    pthread_mutex_lock(mutex);
    VkResult res = dev->fns.queue_bind_sparse(queue, count, binds, fence);
    pthread_mutex_unlock(mutex);
    return res;
}

/* Every queue idle and externally synchronised: the presentation threads must have nothing
 * queued, and the shared queue must not be in use by the layer. */
VKAPI_ATTR VkResult VKAPI_CALL afmf_DeviceWaitIdle(VkDevice device)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL)
        return VK_ERROR_DEVICE_LOST;
    afmf_swapchain_drain_all(dev);
    if (dev->async_queue == VK_NULL_HANDLE)
        return dev->fns.device_wait_idle(device);
    /* DeviceWaitIdle externally synchronizes all queues; no application queue
     * submissions may race this call. Layer workers were drained above. */
    pthread_mutex_lock(&dev->async_lock);
    for (uint32_t i=0;i<dev->queue_count;i++)
        if(dev->queues[i].handle != dev->async_queue) pthread_mutex_lock(&dev->queues[i].lock);
    VkResult res = dev->fns.device_wait_idle(device);
    for (uint32_t i=dev->queue_count;i-->0;)
        if(dev->queues[i].handle != dev->async_queue) pthread_mutex_unlock(&dev->queues[i].lock);
    pthread_mutex_unlock(&dev->async_lock);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL afmf_QueueWaitIdle(VkQueue queue)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(queue));
    if (dev == NULL)
        return VK_ERROR_DEVICE_LOST;
    pthread_mutex_t *mutex=afmf_device_queue_mutex(dev,queue);
    pthread_mutex_lock(mutex);
    VkResult res = dev->fns.queue_wait_idle(queue);
    pthread_mutex_unlock(mutex);
    return res;
}

VKAPI_ATTR void VKAPI_CALL afmf_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    if (device == VK_NULL_HANDLE)
        return;

    struct afmf_device *dev = afmf_device_take(dispatch_key(device));
    if (dev == NULL) {
        AFMF_ERR("vkDestroyDevice on device %p the layer never created", (void *)device);
        return;
    }
    afmf_swapchain_forget_all(dev);
    afmf_framegen_pipelines_destroy(dev);
    dev->fns.destroy_device(device, alloc);
    afmf_device_free(dev);
}

VKAPI_ATTR void VKAPI_CALL afmf_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                                               VkQueue *queue)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL) {
        *queue = VK_NULL_HANDLE;
        return;
    }
    dev->fns.get_device_queue(device, family, index, queue);
    afmf_device_record_queue(dev, *queue, family);
}

VKAPI_ATTR void VKAPI_CALL afmf_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info,
                                                VkQueue *queue)
{
    struct afmf_device *dev = afmf_device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.get_device_queue2 == NULL) {
        *queue = VK_NULL_HANDLE;
        return;
    }
    dev->fns.get_device_queue2(device, info, queue);
    afmf_device_record_queue(dev, *queue, info->queueFamilyIndex);
}

/* The swapchain's generation resources: the images, semaphores, fences and command pool one
 * swapchain's generation runs on, and their teardown. */

#include "swapchain_internal.h"

/* Everything that does not depend on the presenting queue's family. */
VkResult afmf_sc_gen_init(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    VkResult res = dev->fns.get_swapchain_images(dev->handle, sc->handle, &sc->image_count, NULL);
    if (res != VK_SUCCESS)
        return res;
    sc->images = calloc(sc->image_count, sizeof *sc->images);
    sc->sem_generated = calloc(sc->image_count, sizeof *sc->sem_generated);
    sc->sem_real = calloc(sc->image_count, sizeof *sc->sem_real);
    sc->sem_app = calloc(sc->image_count, sizeof *sc->sem_app);
    sc->slots = calloc(sc->image_count, sizeof *sc->slots);
    if (sc->images == NULL || sc->sem_generated == NULL || sc->sem_real == NULL ||
        sc->sem_app == NULL || sc->slots == NULL)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    res = dev->fns.get_swapchain_images(dev->handle, sc->handle, &sc->image_count, sc->images);
    if (res != VK_SUCCESS)
        return res;

    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (uint32_t i = 0; i < sc->image_count; i++) {
        res = dev->fns.create_semaphore(dev->handle, &semaphore, NULL, &sc->sem_generated[i]);
        if (res == VK_SUCCESS)
            res = dev->fns.create_semaphore(dev->handle, &semaphore, NULL, &sc->sem_real[i]);
        if (res == VK_SUCCESS)
            res = dev->fns.create_semaphore(dev->handle, &semaphore, NULL, &sc->sem_app[i]);
        if (res == VK_SUCCESS)
            res = dev->fns.create_fence(dev->handle, &fence, NULL, &sc->slots[i].fence);
        if (res != VK_SUCCESS)
            return res;
    }
    res = dev->fns.create_fence(dev->handle, &fence, NULL, &sc->spare_fence);
    if (res != VK_SUCCESS)
        return res;
    res=afmf_sc_output_init(dev,sc);
    if(res!=VK_SUCCESS) return res;
    if (afmf_config_get()->interpolate)
        sc->fg = afmf_framegen_create(dev, sc->format, sc->extent, sc->image_count, sc->images,
                                      sc->image_count, false, sc->sampled_usage, &sc->config);
    return sc->fg != NULL ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
}

/* The command pool needs the presenting queue's family, only known at the first present. */
bool afmf_sc_ensure_pool(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t family)
{
    if (sc->pool != VK_NULL_HANDLE)
        return sc->pool_family == family;

    const VkQueueFlags copy_capable =
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    if (family >= dev->queue_family_count ||
        (dev->queue_families[family].queueFlags & copy_capable) == 0) {
        AFMF_WARN("swapchain %p: presenting queue family %u cannot copy images", (void *)sc->handle,
                  family);
        return false;
    }

    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    if (dev->fns.create_command_pool(dev->handle, &pool, NULL, &sc->pool) != VK_SUCCESS)
        return false;
    sc->pool_family = family;

    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = sc->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (dev->set_loader_data == NULL) {
        AFMF_WARN("swapchain %p: loader offered no pfnSetDeviceLoaderData", (void *)sc->handle);
        return false;
    }
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (dev->fns.allocate_command_buffers(dev->handle, &alloc, &sc->slots[i].cmd) != VK_SUCCESS)
            return false;
        /* Allocated below the loader's trampoline: stamp the dispatch pointer ourselves. */
        if (dev->set_loader_data(dev->handle, sc->slots[i].cmd) != VK_SUCCESS)
            return false;
    }
    if(!afmf_sc_output_pool(dev,sc,family)) return false;
    if (sc->fg != NULL)
        afmf_framegen_set_family(dev, sc->fg, family);
    return true;
}

void afmf_sc_gen_teardown(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    const struct afmf_device_fns *f = &dev->fns;
    afmf_sc_presenter_stop(sc); /* drains what is queued: the swapchain is still alive here */
    if (sc->async) {
        /* The layer's queue may still be presenting from this swapchain. */
        pthread_mutex_lock(&dev->async_lock);
        (void)f->queue_wait_idle(dev->async_queue);
        pthread_mutex_unlock(&dev->async_lock);
        if (sc->present_queue != VK_NULL_HANDLE && sc->present_queue != dev->async_queue) {
            pthread_mutex_lock(sc->present_lock);
            (void)f->queue_wait_idle(sc->present_queue);
            pthread_mutex_unlock(sc->present_lock);
        }
    }
    if (sc->slots != NULL) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            struct afmf_slot *slot = &sc->slots[i];
            if (slot->pending)
                (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
            if (slot->fence != VK_NULL_HANDLE)
                f->destroy_fence(dev->handle, slot->fence, NULL);
        }
    }
    if (sc->spare_fence != VK_NULL_HANDLE) {
        /* A spare never presented still has its release pending; bounded, a compositor that never
         * answers must not hang the application's teardown. */
        if (sc->spare_valid &&
            f->wait_for_fences(dev->handle, 1, &sc->spare_fence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
            AFMF_WARN("swapchain %p: spare image never released", (void *)sc->handle);
        f->destroy_fence(dev->handle, sc->spare_fence, NULL);
        sc->spare_fence = VK_NULL_HANDLE;
        sc->spare_valid = false;
    }
    if (sc->pool != VK_NULL_HANDLE)
        f->destroy_command_pool(dev->handle, sc->pool, NULL); /* frees the command buffers */
    afmf_sc_output_destroy(dev,sc);
    afmf_framegen_destroy(dev, sc->fg);
    sc->fg = NULL;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->sem_generated != NULL && sc->sem_generated[i] != VK_NULL_HANDLE)
            f->destroy_semaphore(dev->handle, sc->sem_generated[i], NULL);
        if (sc->sem_real != NULL && sc->sem_real[i] != VK_NULL_HANDLE)
            f->destroy_semaphore(dev->handle, sc->sem_real[i], NULL);
        if (sc->sem_app != NULL && sc->sem_app[i] != VK_NULL_HANDLE)
            f->destroy_semaphore(dev->handle, sc->sem_app[i], NULL);
    }
    if (sc->history != VK_NULL_HANDLE)
        f->destroy_image(dev->handle, sc->history, NULL);
    if (sc->history_memory != VK_NULL_HANDLE)
        f->free_memory(dev->handle, sc->history_memory, NULL);
    free(sc->slots);
    free(sc->sem_real);
    free(sc->sem_app);
    free(sc->sem_generated);
    free(sc->images);
    sc->slots = NULL;
    sc->sem_real = NULL;
    sc->sem_app = NULL;
    sc->sem_generated = NULL;
    sc->images = NULL;
    sc->pool = VK_NULL_HANDLE;
    sc->history = VK_NULL_HANDLE;
    sc->history_memory = VK_NULL_HANDLE;
    sc->gen_enabled = false;
}

/* Stops generating on a live swapchain. Its semaphores may still be awaited by the presentation
 * engine, so the resources stay until the swapchain is destroyed. */
void afmf_sc_gen_disable(struct afmf_swapchain *sc, const char *why)
{
    AFMF_WARN("swapchain %p: %s; pass-through from now on", (void *)sc->handle, why);
    afmf_sc_presenter_stop(sc);
    sc->gen_enabled = false;
}

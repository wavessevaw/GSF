/* Frame generation at present time: the hook the application's vkQueuePresentKHR lands in. The
 * pieces live in the swapchain_*.c files next to it; this file is the present itself, threaded
 * (the hook only consumes the application's semaphores and queues the frame) or inline. */

#include "swapchain_internal.h"

/* The application's present, with the generated frame in front of it when one could be made. */
VkResult afmf_sc_present_generated(struct afmf_device *dev, struct afmf_swapchain *sc, VkQueue queue,
                                   uint32_t family, const VkPresentInfoKHR *info, uint64_t ordinal)
{
    const struct afmf_device_fns *f = &dev->fns;
    uint32_t i = info->pImageIndices[0];

    /* With a queue of its own the layer records for that family and presents from it; the
     * application's queue is never waited on. */
    if (sc->present_queue != VK_NULL_HANDLE && sc->present_queue != queue) {
        afmf_sc_presenter_drain(sc);
        return afmf_device_queue_present(dev, queue, info);
    }
    sc->present_queue = queue;
    sc->present_lock = afmf_device_queue_mutex(dev,queue);
    VkQueue work_queue = sc->async ? dev->async_queue : queue;
    if (sc->async)
        family = dev->async_family;

    if (!afmf_sc_ensure_pool(dev, sc, family) || i >= sc->image_count ||
        info->waitSemaphoreCount > AFMF_MAX_APP_WAITS) {
        afmf_sc_gen_disable(sc, "cannot generate on this present path");
        return afmf_device_queue_present(dev, queue, info);
    }

    struct afmf_present_job job = {.real_image = i, .ordinal = ordinal};
    bool threaded = afmf_sc_presenter_start(sc) && afmf_sc_job_from_chain(sc, info, &job);
    if (!threaded) {
        afmf_sc_presenter_drain(sc);
        return afmf_device_queue_present(dev, queue, info);
    }

    struct timespec t_start, t_end;
    (void)clock_gettime(CLOCK_MONOTONIC, &t_start);
    job.arrival = t_start;
    uint32_t slot_index = sc->slot_index;
    sc->slot_index = (sc->slot_index + 1) % sc->image_count;

    if (threaded) {
        /* A slot comes round again after image_count frames, and the presentation thread waits
         * on its fence for the pacing: that frame has to be done before the work thread resets
         * it, so at most image_count frames are in flight between here and their presents. */
        pthread_mutex_lock(&sc->job_lock);
        while (sc->in_flight >= afmf_config_get()->max_in_flight && !sc->presenter_stop)
            pthread_cond_wait(&sc->drain_cond, &sc->job_lock);
        sc->in_flight++;
        pthread_mutex_unlock(&sc->job_lock);

        /* The application's semaphores are consumed before the call returns, as it expects:
         * one submission with nothing to run turns them into the slot's semaphore, which the
         * work thread's submission waits on. */
        VkPipelineStageFlags stages[AFMF_MAX_APP_WAITS];
        for (uint32_t k = 0; k < info->waitSemaphoreCount; k++)
            stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo adapter = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = info->waitSemaphoreCount,
            .pWaitSemaphores = info->pWaitSemaphores,
            .pWaitDstStageMask = stages,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &sc->sem_app[slot_index],
        };
        pthread_mutex_lock(&dev->async_lock);
        VkResult res = f->queue_submit(work_queue, 1, &adapter, VK_NULL_HANDLE);
        pthread_mutex_unlock(&dev->async_lock);
        if (res != VK_SUCCESS) {
            /* Nothing consumed: the application's own present still works. */
            pthread_mutex_lock(&sc->job_lock);
            sc->in_flight--;
            pthread_cond_broadcast(&sc->drain_cond);
            pthread_mutex_unlock(&sc->job_lock);
            sc->slot_index = slot_index;
            afmf_sc_gen_disable(sc, "layer submission failed");
            return afmf_device_queue_present(dev, queue, info);
        }

        struct afmf_frame_job work = {.slot = slot_index, .present = job};
        pthread_mutex_lock(&sc->work_lock);
        while (sc->work_count == AFMF_MAX_JOBS)
            pthread_cond_wait(&sc->work_drain_cond, &sc->work_lock);
        sc->work[(sc->work_head + sc->work_count) % AFMF_MAX_JOBS] = work;
        sc->work_count++;
        pthread_cond_broadcast(&sc->work_cond);
        pthread_mutex_unlock(&sc->work_lock);

        /* The results of earlier presents come back here; the application also learns
         * OUT_OF_DATE from its acquire. */
        pthread_mutex_lock(&sc->job_lock);
        res = sc->deferred_result;
        sc->deferred_result = VK_SUCCESS;
        bool failed = sc->worker_failed;
        pthread_mutex_unlock(&sc->job_lock);
        if (info->pResults != NULL)
            info->pResults[0] = res;
        if (failed)
            afmf_sc_gen_disable(sc, "layer submission failed"); /* after this frame's presents */

        (void)clock_gettime(CLOCK_MONOTONIC, &t_end);
        sc->hook_ms += elapsed_ms(&t_start, &t_end);
        return res;
    }

    return VK_ERROR_INITIALIZATION_FAILED; /* threaded path returned above */
}

VkResult afmf_swapchain_present(struct afmf_device *dev, VkQueue queue, const VkPresentInfoKHR *info)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = info->swapchainCount == 1 ? find_locked(dev, info->pSwapchains[0])
                                                          : NULL;
    pthread_mutex_unlock(&dev->lock);
    if (sc == NULL) {
        for (uint32_t k = 0; k < info->swapchainCount; k++) {
            pthread_mutex_lock(&dev->lock);
            struct afmf_swapchain *each = find_locked(dev, info->pSwapchains[k]);
            pthread_mutex_unlock(&dev->lock);
            if (each != NULL)
                afmf_sc_update_cadence(dev, each);
        }
        return afmf_device_queue_present(dev, queue, info);
    }

    uint64_t ordinal = afmf_sc_update_cadence(dev, sc);
    uint32_t family;
    if (!sc->gen_enabled || !afmf_device_queue_family(dev, queue, &family))
        return afmf_device_queue_present(dev, queue, info);
    return afmf_sc_present_generated(dev, sc, queue, family, info, ordinal);
}

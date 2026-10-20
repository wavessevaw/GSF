/* The work thread: the slot's fence, the governor, the spare, recording and the submission run
 * here, so the hook only consumes the application's semaphores and queues. */

#include "swapchain_internal.h"

/* The frame's work behind the hook: the slot's fence, the governor, the spare, recording and
 * the submission, which waits on `waits` (the application's semaphores inline; from the work
 * thread the slot's adapter semaphore, signalled once they are). Fills the job's companion and
 * pacing; the slot is pending on success. Inline (`threaded` false) the swapchain lock and the
 * layer's queue lock stay held on success for the presents that follow, in the thread's order. */
VkResult afmf_sc_frame_generate(struct afmf_device *dev, struct afmf_swapchain *sc, VkQueue queue,
                                uint32_t slot_index, const VkSemaphore *waits, uint32_t wait_count,
                                bool threaded, struct afmf_present_job *job)
{
    const struct afmf_device_fns *f = &dev->fns;
    struct afmf_slot *slot = &sc->slots[slot_index];
    uint32_t i = job->real_image;
    struct timespec t_start, t_fence, t_acquire, t_record, t_submit;
    (void)clock_gettime(CLOCK_MONOTONIC, &t_start);

    if (slot->pending) {
        VkResult retired = f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        if (retired != VK_SUCCESS) return retired;
        retired = f->reset_fences(dev->handle, 1, &slot->fence);
        if (retired != VK_SUCCESS) return retired;
        slot->pending = false;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_fence);

    /* The image the generated frame goes into: the spare, if the presentation engine has one
     * for us; otherwise this frame simply gets no companion, the application never waits. */
    bool generate = false;
    uint32_t j = 0;
    uint64_t b_ns = (uint64_t)job->arrival.tv_sec * 1000000000ull + (uint64_t)job->arrival.tv_nsec;
    if (sc->plan_epoch_ns == 0) sc->plan_epoch_ns = b_ns;
    afmf_sc_live_update(dev,sc,b_ns);
    const struct afmf_config *cfg=&sc->config;
    job->config=*cfg;
    job->source_a=sc->source_a_ns;job->source_b=b_ns;
    struct gsf_output_plan plan = gsf_output_plan(sc->source_a_ns, b_ns, sc->plan_epoch_ns,
                                                 cfg->refresh_hz, cfg->multiplier);
    job->real_slot=plan.real_slot;
    job->source_period_ns = sc->source_a_ns && b_ns > sc->source_a_ns ? b_ns - sc->source_a_ns : 0;
    sc->source_a_ns = b_ns;
    pthread_mutex_lock(&sc->job_lock);
    bool allowed = cfg->enabled && !sc->recreate_requested && afmf_sc_governor_allows(sc, sc->last_delay_ms, job->ordinal);
    uint64_t tick=gsf_refresh_period(cfg->refresh_hz);
    job->buffer_delay_ns=sc->frame_ms_ema>0 ? (uint64_t)(sc->frame_ms_ema*1e6) : 2*tick;
    if(job->buffer_delay_ns>100000000ull) job->buffer_delay_ns=100000000ull;
    bool have_history = sc->have_history;
    if (!have_history)
        sc->skipped_no_history++;
    pthread_mutex_unlock(&sc->job_lock);
    if (have_history && allowed) {
        for (uint32_t n = 0; n < plan.count; n++) {
            if (!afmf_sc_spare_take(dev, sc, &j)) break;
            job->output_images[job->output_count] = j;
            job->phases[job->output_count] = plan.phases[n];
            job->output_slots[job->output_count] = plan.slots[n];
            job->output_count++;
        }
        generate = job->output_count > 0;
        if (!generate) {
            pthread_mutex_lock(&sc->job_lock);
            sc->skipped_no_image++;
            pthread_mutex_unlock(&sc->job_lock);
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_acquire);

    VkResult res = afmf_sc_record_frame(dev, sc, slot->cmd, slot_index, i, job->output_images, job->phases, job->output_count);
    (void)clock_gettime(CLOCK_MONOTONIC, &t_record);
    if (res == VK_SUCCESS) {
        VkPipelineStageFlags stages[AFMF_MAX_APP_WAITS];
        for (uint32_t k = 0; k < wait_count; k++)
            stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSemaphore signals[1 + GSF_NEXT_MAX_OUTPUTS] = {sc->sem_real[i]};
        for (uint32_t n = 0; n < job->output_count; n++)
            signals[n + 1] = sc->sem_generated[job->output_images[n]];
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = wait_count,
            .pWaitSemaphores = waits,
            .pWaitDstStageMask = stages,
            .commandBufferCount = 1,
            .pCommandBuffers = &slot->cmd,
            .signalSemaphoreCount = job->output_count + 1u,
            .pSignalSemaphores = signals,
        };
        if (!threaded)
            wsi_take(sc);
        if (sc->async)
            pthread_mutex_lock(&dev->async_lock);
        res = getenv("GSF_NEXT_TEST_FAIL_SUBMIT") ? VK_ERROR_DEVICE_LOST :
            f->queue_submit(queue, 1, &submit, slot->fence);
        if (sc->async && (res != VK_SUCCESS || threaded))
            pthread_mutex_unlock(&dev->async_lock);
        if (!threaded && res != VK_SUCCESS)
            wsi_give(sc);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_submit);

    pthread_mutex_lock(&sc->job_lock);
    sc->last_gpu_ms = cfg->enabled ? afmf_framegen_gpu_ms(sc->fg) : 0;
    afmf_framegen_gpu_stages(sc->fg, &sc->last_motion_ms, sc->last_warp_ms);
    if(!cfg->enabled) { sc->last_motion_ms=0;memset(sc->last_warp_ms,0,sizeof sc->last_warp_ms); }
    sc->fence_ms += elapsed_ms(&t_start, &t_fence);
    sc->acquire_ms += elapsed_ms(&t_fence, &t_acquire);
    sc->record_ms += elapsed_ms(&t_acquire, &t_record);
    sc->submit_ms += elapsed_ms(&t_record, &t_submit);
    if (res == VK_SUCCESS) {
        sc->have_history = cfg->enabled;
        if (generate)
            sc->generated += job->output_count;
    }
    pthread_mutex_unlock(&sc->job_lock);
    if (res != VK_SUCCESS)
        return res; /* the image acquired for the generated frame, if any, stays with the engine */

    slot->pending = true;
    job->done = slot->fence;

    /* Debug dumps block on the submission; only while AFMF_DUMP_DIR asks for frames. */
    if (sc->fg != NULL)
        afmf_framegen_dump_arm(sc->fg);
    if (generate && sc->fg != NULL && afmf_framegen_dump_recorded(sc->fg)) {
        (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        afmf_framegen_dump_write(dev, sc->fg);
        /* Source arrival is immutable even when debug readbacks stall the worker. */
    }

    return VK_SUCCESS;
}

void *afmf_sc_worker_main(void *arg)
{
    struct afmf_swapchain *sc = arg;
    struct afmf_device *dev = sc->dev;

    pthread_mutex_lock(&sc->work_lock);
    for (;;) {
        while (sc->work_count == 0 && !sc->worker_stop)
            pthread_cond_wait(&sc->work_cond, &sc->work_lock);
        if (sc->work_count == 0)
            break; /* stopping, and drained */
        struct afmf_frame_job work = sc->work[sc->work_head];
        sc->worker_busy = true;
        pthread_mutex_unlock(&sc->work_lock);

        VkResult res = afmf_sc_frame_generate(dev, sc, dev->async_queue, work.slot,
                                              &sc->sem_app[work.slot], 1, true, &work.present);
        if (res != VK_SUCCESS) {
            /* The hook already consumed the application's semaphores into the slot's: the real
             * frame's present still needs sem_real, or it waits forever. One submission with
             * nothing to run passes the signal on; if that fails too, the present goes out
             * without its wait rather than never. Generation stops at the hook's next call. */
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo pass = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &sc->sem_app[work.slot],
                .pWaitDstStageMask = &stage,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &sc->sem_real[work.present.real_image],
            };
            pthread_mutex_lock(&dev->async_lock);
            VkResult passed = getenv("GSF_NEXT_TEST_FAIL_SUBMIT") ? VK_ERROR_DEVICE_LOST :
                dev->fns.queue_submit(dev->async_queue, 1, &pass, sc->slots[work.slot].fence);
            pthread_mutex_unlock(&dev->async_lock);
            if(passed==VK_SUCCESS) {
                sc->slots[work.slot].pending=true;
                work.present.done=sc->slots[work.slot].fence;
            }
            work.present.output_count = 0;
            pthread_mutex_lock(&sc->job_lock);
            if (sc->deferred_result >= 0)
                sc->deferred_result = res;
            sc->worker_failed = true;
            pthread_cond_broadcast(&sc->job_cond);
            if (passed != VK_SUCCESS) {
                sc->deferred_result = passed;
                sc->in_flight--;
                pthread_cond_broadcast(&sc->drain_cond);
            }
            pthread_mutex_unlock(&sc->job_lock);
            if (passed != VK_SUCCESS) {
                /* No safe semaphore signal exists: return the device/submission error,
                 * retain acquired images for teardown, and never enqueue an unsafe present. */
                pthread_mutex_lock(&sc->work_lock);
                sc->work_head = (sc->work_head + 1) % AFMF_MAX_JOBS;
                sc->work_count--;
                sc->worker_busy = false;
                pthread_cond_broadcast(&sc->work_drain_cond);
                continue;
            }
        }

        /* Hand both presents to the presentation thread. */
        pthread_mutex_lock(&sc->job_lock);
        while (sc->job_count == AFMF_MAX_JOBS)
            pthread_cond_wait(&sc->drain_cond, &sc->job_lock);
        sc->jobs[(sc->job_head + sc->job_count) % AFMF_MAX_JOBS] = work.present;
        sc->job_count++;
        pthread_cond_broadcast(&sc->job_cond);
        pthread_mutex_unlock(&sc->job_lock);

        pthread_mutex_lock(&sc->work_lock);
        sc->work_head = (sc->work_head + 1) % AFMF_MAX_JOBS;
        sc->work_count--;
        sc->worker_busy = false;
        pthread_cond_broadcast(&sc->work_drain_cond);
    }
    pthread_mutex_unlock(&sc->work_lock);
    return NULL;
}

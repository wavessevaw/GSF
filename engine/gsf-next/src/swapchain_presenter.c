/* The presentation thread: GPU-ready intermediates followed by the real endpoint,
 * on a bounded monotonic cadence; late sequences shift together. Under Proton each vkQueuePresentKHR measured 170-400 us of host time on
 * the application's thread; both presents run here instead. */

#include "swapchain_internal.h"
#include "presentation_deadline.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

static uint64_t monotonic_ns(void)
{
    struct timespec now;(void)clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
}

static void pace_until(uint64_t deadline)
{
    struct timespec until={.tv_sec=(time_t)(deadline/1000000000ull),.tv_nsec=(long)(deadline%1000000000ull)};
    while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&until,NULL)==EINTR) ;
}

/* Optional local evidence: actual accepted API call intervals, NOT scanout. */
static void trace_present(struct afmf_swapchain *sc,const struct afmf_present_job *job,
                          bool real,float phase,uint64_t target,uint64_t start,VkResult result)
{
    const char *base=getenv("GSF_NEXT_PRESENT_REPORT");
    if(base==NULL || !job->config.diagnostics) return;
    char path[4096];
    int n=snprintf(path,sizeof path,"%s.%ld.%llu.jsonl",base,(long)getpid(),(unsigned long long)sc->plan_epoch_ns);
    if(n<0 || (size_t)n>=sizeof path) return;
    struct stat st;
    if(stat(path,&st)==0 && st.st_size>16*1024*1024) {
        char backup[4100];(void)snprintf(backup,sizeof backup,"%s.1",path);(void)rename(path,backup);
    }
    FILE *out=fopen(path,"a");if(out==NULL) return;
    (void)fprintf(out,"{\"source_b\":%llu,\"source_a\":%llu,\"ordinal\":%llu,\"real\":%s,\"t\":%.6f,\"target_ns\":%llu,\"submit_ns\":%llu,\"ready_observed_ns\":%llu,\"result\":%d,\"refresh_hz\":%u,\"revision\":%llu,\"multiplier\":%u,\"enabled\":%s,\"quality\":%d,\"queue_limit\":%u}\n",
        (unsigned long long)job->source_b,(unsigned long long)job->source_a,(unsigned long long)job->ordinal,
        real?"true":"false",(double)phase,(unsigned long long)target,(unsigned long long)start,
        (unsigned long long)job->ready_ns,(int)result,job->config.refresh_hz,(unsigned long long)job->config.revision,job->config.multiplier,job->config.enabled?"true":"false",
        (int)job->config.performance,job->config.max_in_flight);
    (void)fclose(out);
}

/* One present from the layer's queue, with the parts of the application's chain that were kept.
 * The present id only goes on the real frame: ids must increase, and the companion has none. */
static VkResult present_one(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t image,
                            VkSemaphore wait, const struct afmf_present_job *job, bool real,uint64_t desired,float phase)
{
    VkPresentIdKHR id = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &job->present_id,
    };
#ifdef VK_KHR_present_id2
    VkPresentId2KHR id2 = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .swapchainCount = 1,
        .pPresentIds = &job->present_id,
    };
#endif
    VkSwapchainPresentModeInfoEXT mode = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .swapchainCount = 1,
        .pPresentModes = &job->present_mode,
    };
    VkSwapchainPresentFenceInfoEXT fence = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .swapchainCount = 1,
        .pFences = &job->present_fence,
    };
#ifdef VK_EXT_present_timing
    VkPresentTimingsInfoEXT timings = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
        .swapchainCount = 1,
        .pTimingInfos = &job->timing,
    };
#endif
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1u : 0u,
        .pWaitSemaphores = &wait,
        .swapchainCount = 1,
        .pSwapchains = &sc->handle,
        .pImageIndices = &image,
    };
    const void **tail = &present.pNext;
    if (job->have_present_mode) {
        *tail = &mode;
        tail = &mode.pNext;
    }
    if (real && job->have_present_id) {
#ifdef VK_KHR_present_id2
        if (job->present_id_v2) {
            *tail = &id2;
            tail = &id2.pNext;
        } else
#endif
        {
            *tail = &id;
            tail = &id.pNext;
        }
    }
    if (real && job->present_fence != VK_NULL_HANDLE) {
        *tail = &fence;
        tail = &fence.pNext;
    }
#ifdef VK_EXT_present_timing
    if (real && job->have_timing)
        *tail = &timings;
#endif

    uint64_t target=desired;
    bool paced=job->config.enabled && job->config.pacing;
    uint64_t hold_start=monotonic_ns();
    if(paced) {
        target=gsf_grid_deadline(sc->plan_epoch_ns,desired,hold_start,sc->last_target_ns,sc->last_output_ns,job->config.refresh_hz);
        pace_until(target);
    }
    struct timespec t0, t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    wsi_take(sc);
    pthread_mutex_lock(sc->present_lock);
    uint64_t submitted=monotonic_ns();
    VkResult res = dev->fns.queue_present(sc->present_queue, &present);
    pthread_mutex_unlock(sc->present_lock);
    wsi_give(sc);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Thread-side counters live under job_lock (never dev->lock: vkDeviceWaitIdle drains the
     * thread while holding dev->lock). */
    pthread_mutex_lock(&sc->job_lock);
    sc->hold_ms+=(double)(submitted-hold_start)/1e6;
    if (real)
        sc->present_real_ms += elapsed_ms(&t0, &t1);
    else
        sc->present_ms += elapsed_ms(&t0, &t1);
    pthread_mutex_unlock(&sc->job_lock);
    pthread_mutex_lock(&sc->job_lock);
    if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
        sc->last_present_gap_ns=sc->last_output_ns ? submitted-sc->last_output_ns : 0;
        sc->last_output_ns=submitted;
        if(paced) {sc->paced_presents++;sc->last_target_ns=target;}
        if (real) {
            sc->accepted_real++;sc->presented_config=job->config;
            if(job->have_present_id) sc->last_submitted_present_id=job->present_id;
            pthread_cond_broadcast(&sc->job_cond);
        } else sc->accepted_synthetic++;
    }
    pthread_mutex_unlock(&sc->job_lock);
    trace_present(sc,job,real,phase,target,submitted,res);
    return res;
}

void *afmf_sc_presenter_main(void *arg)
{
    struct afmf_swapchain *sc = arg;
    struct afmf_device *dev = sc->dev;

    pthread_mutex_lock(&sc->job_lock);
    for (;;) {
        while (sc->job_count == 0 && !sc->presenter_stop)
            pthread_cond_wait(&sc->job_cond, &sc->job_lock);
        if (sc->job_count == 0)
            break; /* stopping, and drained */
        struct afmf_present_job job = sc->jobs[sc->job_head];
        VkResult first=VK_SUCCESS,second=VK_SUCCESS;
        pthread_mutex_unlock(&sc->job_lock);
        if(job.early_real) {
            /* Stage 1 alone has finished reading B and restored PRESENT_SRC.
             * Optical flow in stage 2 is optional and MUST NOT gate real B.
             * The presenter keeps output order G then B only for on-time G. */
            VkResult ingest;
            do { ingest=dev->fns.wait_for_fences(dev->handle,1,&job.ingest_done,
                                                VK_TRUE,5000000ull); }
            while(ingest==VK_TIMEOUT);
            job.ready_ns=monotonic_ns(); /* real ingestion, not generation-ready */
            if(ingest!=VK_SUCCESS) {
                second=ingest;
                pthread_mutex_lock(&sc->job_lock);
                sc->worker_failed=true;
                pthread_mutex_unlock(&sc->job_lock);
            } else {
                uint64_t tick=gsf_refresh_period(job.config.refresh_hz);
                uint64_t shift=job.config.enabled ? job.buffer_delay_ns:0;
                uint64_t real_due=job.real_slot+shift;
                bool synth_ready=false;
                uint32_t shown=0;
                for(uint32_t n=0;n<job.output_count;n++) {
                    uint64_t desired=job.output_slots[n]+shift;
                    uint64_t now=monotonic_ns();
                    pthread_mutex_lock(&sc->job_lock);
                    bool superseded=sc->job_count>1;
                    pthread_mutex_unlock(&sc->job_lock);
                    enum gsf_synthetic_deadline policy=gsf_synthetic_deadline_check(
                        sc->plan_epoch_ns,desired,real_due,now,
                        sc->last_target_ns,sc->last_output_ns,tick,
                        job.config.refresh_hz,superseded);
                    if(policy!=GSF_SYNTHETIC_ON_TIME) {
                        pthread_mutex_lock(&sc->job_lock);
                        sc->dropped_synthetic+=job.output_count-n;
                        if(policy==GSF_SYNTHETIC_SUPERSEDED)
                            sc->superseded_synthetic+=job.output_count-n;
                        else sc->expired_synthetic+=job.output_count-n;
                        pthread_mutex_unlock(&sc->job_lock);
                        break;
                    }
                    /* If the synthetic GPU work isn't ready by the slot,
                     * discard the OPTIONAL image. Never block B behind it. */
                    if(now<desired) pace_until(desired);
                    VkResult gpu=dev->fns.wait_for_fences(dev->handle,1,&job.done,VK_TRUE,0);
                    if(gpu!=VK_SUCCESS) {
                        pthread_mutex_lock(&sc->job_lock);
                        sc->dropped_synthetic+=job.output_count-n;
                        sc->expired_synthetic+=job.output_count-n;
                        pthread_mutex_unlock(&sc->job_lock);
                        break;
                    }
                    if(!synth_ready) {
                        synth_ready=true;
                        job.ready_ns=monotonic_ns();
                        pthread_mutex_lock(&sc->job_lock);
                        sc->ready_synthetic+=job.output_count;
                        pthread_mutex_unlock(&sc->job_lock);
                    }
                    uint32_t image=0;
                    VkResult copied=afmf_sc_output_copy(dev,sc,&job,n,&image);
                    if(copied==VK_NOT_READY) {
                        pthread_mutex_lock(&sc->job_lock);
                        sc->dropped_synthetic+=job.output_count-n;
                        pthread_mutex_unlock(&sc->job_lock);
                        break;
                    }
                    if(copied!=VK_SUCCESS) {
                        pthread_mutex_lock(&sc->job_lock);
                        sc->dropped_synthetic+=job.output_count-n;
                        pthread_mutex_unlock(&sc->job_lock);
                        first=copied;
                        break;
                    }
                    VkResult presented=present_one(dev,sc,image,sc->sem_generated[image],
                                                  &job,false,desired,job.phases[n]);
                    shown++;
                    if(presented<0 || first==VK_SUCCESS) first=presented;
                    if(presented<0) break;
                }
                (void)shown;
                /* Real WSI signal is stage-1 complete, never stage-2. */
                job.ready_ns=monotonic_ns();
                second=present_one(dev,sc,job.real_image,sc->sem_real[job.real_image],
                                   &job,true,real_due,1.0f);
            }
        } else {
        /* Never enqueue a WSI wait before observing ready. The bounded queue
         * owns every resource until this fence retires, including during stop. */
        VkResult gpu;
        do { gpu=dev->fns.wait_for_fences(dev->handle,1,&job.done,VK_TRUE,5000000ull); }
        while(gpu==VK_TIMEOUT);
        job.ready_ns=monotonic_ns();
        const char *test_delay=getenv("GSF_NEXT_TEST_READY_DELAY_MS");
        if(test_delay!=NULL) {
            long delay=strtol(test_delay,NULL,10);
            if(delay>0 && delay<250) { pace_until(job.ready_ns+(uint64_t)delay*1000000ull);job.ready_ns=monotonic_ns(); }
        }
        pthread_mutex_lock(&sc->job_lock);
        double delay_ms=(double)(job.ready_ns-(uint64_t)job.arrival.tv_sec*1000000000ull-(uint64_t)job.arrival.tv_nsec)/1e6;
        sc->gpu_delay_ms+=delay_ms;sc->last_delay_ms=delay_ms;
        if(gpu==VK_SUCCESS) sc->ready_synthetic+=job.output_count;
        pthread_mutex_unlock(&sc->job_lock);
        if(gpu==VK_SUCCESS) {
            uint64_t first_slot=job.output_count ? job.output_slots[0] : job.real_slot;
            uint64_t shift=job.config.enabled ? job.buffer_delay_ns : 0;
            uint64_t earliest=job.ready_ns;
            if(job.config.enabled && job.config.pacing)
                earliest=gsf_grid_deadline(sc->plan_epoch_ns,0,earliest,sc->last_target_ns,sc->last_output_ns,job.config.refresh_hz);
            bool late=first_slot+shift<earliest;
            if(late) {
                pthread_mutex_lock(&sc->job_lock);sc->late_pairs++;pthread_mutex_unlock(&sc->job_lock);
            }
            /* Deadline rescue is opt-in until the source/real GPU semaphore
             * dependency is removed. A trace replay showed that enabling it
             * with the Alpha 4 buffer would suppress nearly all FG. */
            const char *priority_env=getenv("GSF_NEXT_REAL_PRIORITY_PACING");
            bool priority=job.config.enabled && job.config.pacing &&
                          priority_env && priority_env[0]=='1' && priority_env[1]==0;
            uint64_t first_due=late ? earliest : first_slot+shift;
            uint64_t tick=gsf_refresh_period(job.config.refresh_hz);
            uint64_t real_due=job.real_slot+shift;
            uint32_t shown=0;
            for(uint32_t n=0;n<job.output_count;n++) {
                uint64_t desired=priority ? job.output_slots[n]+shift :
                    (late ? first_due+(uint64_t)shown*tick : job.output_slots[n]+shift);
                if(priority) {
                    uint64_t now=monotonic_ns();
                    pthread_mutex_lock(&sc->job_lock);
                    bool newer_real=sc->job_count>1;
                    pthread_mutex_unlock(&sc->job_lock);
                    enum gsf_synthetic_deadline drop=gsf_synthetic_deadline_check(
                        sc->plan_epoch_ns,desired,real_due,now,
                        sc->last_target_ns,sc->last_output_ns,tick,
                        job.config.refresh_hz,newer_real);
                    if(drop!=GSF_SYNTHETIC_ON_TIME) {
                        pthread_mutex_lock(&sc->job_lock);
                        sc->dropped_synthetic+=job.output_count-n;
                        if(drop==GSF_SYNTHETIC_SUPERSEDED)
                            sc->superseded_synthetic+=job.output_count-n;
                        else
                            sc->expired_synthetic+=job.output_count-n;
                        pthread_mutex_unlock(&sc->job_lock);
                        break;
                    }
                }
                uint32_t image=0;
                VkResult result=afmf_sc_output_copy(dev,sc,&job,n,&image);
                if(result==VK_NOT_READY) {
                    pthread_mutex_lock(&sc->job_lock);sc->dropped_synthetic+=job.output_count-n;pthread_mutex_unlock(&sc->job_lock);
                    break;
                }
                if(result!=VK_SUCCESS) {
                    pthread_mutex_lock(&sc->job_lock);sc->dropped_synthetic+=job.output_count-n;pthread_mutex_unlock(&sc->job_lock);
                    first=result;break;
                }
                /* Once WSI has acquired an image, it must be presented or safely
                 * retired. Never abandon it after a late GPU copy. */
                result=present_one(dev,sc,image,sc->sem_generated[image],&job,false,desired,job.phases[n]);
                shown++;
                if(result<0 || first==VK_SUCCESS) first=result;
            }
            second=present_one(dev,sc,job.real_image,sc->sem_real[job.real_image],&job,true,
                priority ? real_due : (late ? first_due+(uint64_t)shown*tick : real_due),1.0f);
        } else {
            second=gpu;
            pthread_mutex_lock(&sc->job_lock);sc->worker_failed=true;pthread_mutex_unlock(&sc->job_lock);
        }

        } /* ordinary shared GPU-fence path */

        /* No speculative WSI acquire after the real endpoint. That could hold
         * an image (and a host lock) away from the game's next source frame. */
        pthread_mutex_lock(&sc->job_lock);
        /* The worst outcome reaches the application with its next present call. Errors beat
         * SUBOPTIMAL, SUBOPTIMAL beats SUCCESS. */
        VkResult worst = first < 0 ? first : second < 0 ? second : first != VK_SUCCESS ? first : second;
        if (sc->deferred_result >= 0 && (worst < 0 || sc->deferred_result == VK_SUCCESS))
            sc->deferred_result = worst;
        sc->job_head = (sc->job_head + 1) % AFMF_MAX_JOBS;
        sc->job_count--;
        sc->in_flight--;
        pthread_cond_broadcast(&sc->job_cond);
        afmf_sc_status_write(sc);
        /* Every waiter re-checks its own condition: the drain wants zero, the hook wants a
         * frame in flight fewer than the images. */
        pthread_cond_broadcast(&sc->drain_cond);
    }
    pthread_mutex_unlock(&sc->job_lock);
    return NULL;
}

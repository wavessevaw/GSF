/* SPDX-License-Identifier: GPL-3.0-or-later
 * Experimental OUTPUT-worker frame-spacing pacer (CLOCK_MONOTONIC).
 *
 * This spaces GPU PRESENT submissions; it does not assert DRM scanout,
 * provide compositor presentation feedback, or synthesize frames. Never
 * call this from the application's vkQueuePresentKHR intercepted callback.
 */
#ifndef GFG_OUTPUT_FRAME_PACER_H
#define GFG_OUTPUT_FRAME_PACER_H
#include <stdint.h>
#include <errno.h>
#include <time.h>

typedef struct {
    uint64_t target_ns,previous_ns,emitted;
    uint64_t gap_min_ns,gap_max_ns,gap_sum_ns;
    uint64_t slept_ns,late_ns,reanchors;
} gfg_output_frame_pacer;

static inline uint64_t gfg_pacer_monotonic_ns(void) {
    struct timespec t={0};
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static inline struct timespec gfg_pacer_to_time(uint64_t ns) {
    return (struct timespec){
        .tv_sec=(time_t)(ns/1000000000ull),
        .tv_nsec=(long)(ns%1000000000ull)};
}
static inline void gfg_output_pacer_init(
        gfg_output_frame_pacer *p,uint32_t target_hz) {
    if(!p)return;
    *p=(gfg_output_frame_pacer){0};
    if(target_hz)p->target_ns=1000000000ull/(uint64_t)target_hz;
}
/* Run immediately before the worker's real WSI vkQueuePresentKHR, after
 * its GPU copy is queued. A late source shifts the next deadline forward
 * rather than emitting backlogged "catch-up" Presents with 0ms spacing.
 * Source FPS and presentation FPS remain independently observable. */
static inline int gfg_output_pacer_before_present(gfg_output_frame_pacer *p) {
    if(!p)return EINVAL;
    uint64_t now=gfg_pacer_monotonic_ns();
    if(!now)return EINVAL;
    if(p->target_ns && p->previous_ns) {
        uint64_t deadline=p->previous_ns+p->target_ns;
        if(now<deadline) {
            const struct timespec absolute=gfg_pacer_to_time(deadline);
            int err=0;
            do {err=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,
                                    &absolute,NULL);}
            while(err==EINTR);
            if(err)return err;
            p->slept_ns+=deadline-now;
        } else {
            p->late_ns+=now-deadline;
            ++p->reanchors;
        }
        now=gfg_pacer_monotonic_ns();
        if(!now)return EINVAL;
    }
    if(p->previous_ns) {
        const uint64_t gap=now-p->previous_ns;
        if(!p->gap_min_ns||gap<p->gap_min_ns)p->gap_min_ns=gap;
        if(gap>p->gap_max_ns)p->gap_max_ns=gap;
        p->gap_sum_ns+=gap;
    }
    p->previous_ns=now;
    ++p->emitted;
    return 0;
}
#endif

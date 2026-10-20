/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "output_plan.h"

/* Only optional synthetic frames may expire. A real endpoint ALWAYS retains
 * its ownership, semaphore and present-id. Compute this BEFORE WSI acquire:
 * an acquired image cannot be silently discarded without correct retirement. */
enum gsf_synthetic_deadline {
    GSF_SYNTHETIC_ON_TIME=0,
    GSF_SYNTHETIC_SUPERSEDED,
    GSF_SYNTHETIC_EXPIRED
};

static inline enum gsf_synthetic_deadline
gsf_synthetic_deadline_check(uint64_t epoch,uint64_t desired,
                             uint64_t real_due,uint64_t now,
                             uint64_t last_target,uint64_t last_output,
                             uint64_t tick,uint32_t hz,bool newer_real)
{
    if(newer_real) return GSF_SYNTHETIC_SUPERSEDED;
    if(tick==0 || hz<30 || hz>360 || desired>=real_due)
        return GSF_SYNTHETIC_EXPIRED;
    /* More than half a refresh too late: advancing it to a new slot merely
     * pushes the already-ready real B out and turns interpolation into stutter. */
    if(now>desired && now-desired>tick/2)
        return GSF_SYNTHETIC_EXPIRED;
    uint64_t due=gsf_grid_deadline(epoch,desired,now,last_target,last_output,hz);
    /* The real endpoint owns its slot. Guard against projected output-grid
     * rebooking, not only the raw timestamp. */
    if(due>=real_due || (real_due-due)<tick)
        return GSF_SYNTHETIC_EXPIRED;
    return GSF_SYNTHETIC_ON_TIME;
}

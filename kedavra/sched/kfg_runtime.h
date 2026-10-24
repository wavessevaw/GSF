// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

// No wall-clock subtraction across domains: all inputs are CPU monotonic ns.
static inline bool kfg_runtime_completed(uint64_t job_epoch, uint64_t epoch, uint64_t seq, uint64_t completed)
{
    return job_epoch == epoch && seq != 0 && seq <= completed;
}

// Reserve the NEXT final frame before admitting optional analysis. A missed target
// is not silently advanced to a later vblank (that would hide deadline misses).
static inline bool kfg_runtime_admit(uint64_t now, uint64_t next_target, uint64_t final_ns,
                                     uint64_t analysis_ns, uint64_t guard_ns)
{
    if (now >= next_target) return false;
    uint64_t slack = next_target - now;
    if (final_ns >= slack) return false;
    slack -= final_ns;
    if (guard_ns >= slack) return false;
    return analysis_ns < slack - guard_ns;
}

static inline bool kfg_runtime_slot_free(bool texture_or_fb_referenced, uint64_t write_seq, uint64_t completed)
{
    return !texture_or_fb_referenced && write_seq <= completed;
}

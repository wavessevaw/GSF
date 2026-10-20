/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* 0 = dynamic queue size. At 90 Hz this grants 3/4/5 jobs for x2/x3/x4.
 * Scale linearly with display refresh; never exceed the resources actually
 * reserved for this swapchain. One additional safety job absorbs scheduling
 * jitter instead of binding immediately at the theoretical minimum.
 * Greater depth deliberately trades latency
 * for absorbing a longer producer / reconstruction schedule. */
static inline uint32_t gsf_queue_desired(uint32_t setting,uint32_t hz,uint32_t multiplier)
{
    if(setting>0u) return setting>8u ? 8u : setting;
    if(multiplier<2u) multiplier=2u;
    uint64_t scaled=((uint64_t)hz*multiplier+89u)/90u+1u;
    if(scaled<2u) scaled=2u;
    return scaled>8u ? 8u:(uint32_t)scaled;
}
static inline uint32_t gsf_queue_effective(uint32_t setting,uint32_t hz,
                                            uint32_t multiplier,uint32_t capacity)
{
    uint32_t requested=gsf_queue_desired(setting,hz,multiplier);
    if(capacity==0u) return 1u; /* caller's swapchain is not initialized */
    return requested>capacity ? capacity:requested;
}

/* Convert queued-resource headroom to a bounded display playout horizon.
 * Keep the ordinary source-period EMA if larger, and never let queue growth
 * create an unbounded delay on low refresh monitors. */
static inline uint64_t gsf_queue_playout_ticks(uint64_t source_ticks,
                                                uint32_t effective_queue)
{
    uint64_t required=(uint64_t)effective_queue+1u;
    if(source_ticks<required) source_ticks=required;
    if(source_ticks>9u) source_ticks=9u;
    return source_ticks;
}

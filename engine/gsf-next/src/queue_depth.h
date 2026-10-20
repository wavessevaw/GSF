/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* 0 = dynamic queue size. At 90 Hz this grants 2/3/4 jobs for x2/x3/x4.
 * Scale linearly with display refresh; never exceed the resources actually
 * reserved for this swapchain. Greater depth deliberately trades latency
 * for absorbing a longer producer / reconstruction schedule. */
static inline uint32_t gsf_queue_desired(uint32_t setting,uint32_t hz,uint32_t multiplier)
{
    if(setting>0u) return setting>8u ? 8u : setting;
    if(multiplier<2u) multiplier=2u;
    uint64_t scaled=((uint64_t)hz*multiplier+89u)/90u;
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

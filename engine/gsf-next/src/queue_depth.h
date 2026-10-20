/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* 0 = conservative automatic fallback. Earlier proportional Auto (3..8) was
 * invalidated by Deck observations: increasing queue depth improved pacing
 * only until an optimum and then accumulated latency/backpressure. Until GSF
 * owns a genuinely independent output swapchain, an FPS/multiplier formula
 * is NOT a safe admission controller. Explicit manual 1..8 remains available
 * for diagnostics and controlled A/B comparisons. This is a containment fix,
 * not a claim of sustained x2 / x3 output throughput. */
static inline uint32_t gsf_queue_desired(uint32_t setting,uint32_t hz,uint32_t multiplier)
{
    (void)hz;
    (void)multiplier;
    if(setting>0u) return setting>8u ? 8u : setting;
    return 2u;
}
static inline uint32_t gsf_queue_effective(uint32_t setting,uint32_t hz,
                                            uint32_t multiplier,uint32_t capacity)
{
    uint32_t requested=gsf_queue_desired(setting,hz,multiplier);
    if(capacity==0u) return 1u; /* caller's swapchain is not initialized */
    return requested>capacity ? capacity:requested;
}

/* Only explicit manual deep queue settings may use this bounded playout
 * horizon. Never call it for the protected Auto setting (0). */
static inline uint64_t gsf_queue_playout_ticks(uint64_t source_ticks,
                                                uint32_t effective_queue)
{
    uint64_t required=(uint64_t)effective_queue+1u;
    if(source_ticks<required) source_ticks=required;
    if(source_ticks>9u) source_ticks=9u;
    return source_ticks;
}

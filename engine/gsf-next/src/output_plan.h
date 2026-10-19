/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

#define GSF_NEXT_MAX_OUTPUTS 3u
struct gsf_output_plan {
    uint32_t count;
    float phases[GSF_NEXT_MAX_OUTPUTS];
    uint64_t slots[GSF_NEXT_MAX_OUTPUTS];
};
/* Slots strictly between the two source timestamps, on one display clock. A real
 * endpoint owns its own slot. Never run optical flow for an empty reservation. */
static inline struct gsf_output_plan gsf_output_plan(uint64_t a, uint64_t b,
                                                     uint64_t epoch, uint32_t hz,
                                                     uint32_t multiplier)
{
    struct gsf_output_plan p = {0};
    if (hz < 30 || hz > 360 || multiplier < 2 || multiplier > 4 ||
        a < epoch || b <= a || b - a > 250000000ull)
        return p;
    uint64_t tick = 1000000000ull / hz;
    /* Source timestamps can differ by sub-microsecond measurement noise from
     * vblank. Reserve a 2% guard at each endpoint to avoid nearly duplicate t. */
    uint64_t guard = tick / 50;
    uint64_t first = (a + guard - epoch) / tick + 1;
    uint64_t last = b > epoch + guard ? (b - epoch - guard) / tick : 0;
    if (last < first) return p;
    uint64_t available = last - first + 1;
    p.count = available < multiplier - 1 ? (uint32_t)available : multiplier - 1;
    for (uint32_t k = 0; k < p.count; k++) {
        uint64_t index = ((uint64_t)(k + 1) * (available + 1)) / (p.count + 1) - 1;
        p.slots[k] = epoch + (first + index) * tick;
        p.phases[k] = (float)(p.slots[k] - a) / (float)(b - a);
    }
    return p;
}

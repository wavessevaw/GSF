/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#define GSF_NEXT_MAX_OUTPUTS 3u
struct gsf_output_plan {
    uint32_t count;
    float phases[GSF_NEXT_MAX_OUTPUTS];
    uint64_t slots[GSF_NEXT_MAX_OUTPUTS];
    uint64_t real_slot;
};
static inline uint64_t gsf_refresh_period(uint32_t hz)
{ return (1000000000ull+hz-1)/hz; }
/* Quantize BOTH real endpoints. Their slots are occupied, not synthetic budget.
 * E.g. 46 real fps at 90 Hz admits ~44 synthetic, not ~90 synthetic + 46 real. */
static inline struct gsf_output_plan gsf_output_plan(uint64_t a,uint64_t b,
                                                     uint64_t epoch,uint32_t hz,uint32_t multiplier)
{
    struct gsf_output_plan p={0};p.real_slot=b;
    if(hz<30 || hz>360 || b<epoch) return p;
    uint64_t tick=gsf_refresh_period(hz);
    /* Nearest grid point avoids a nanosecond roundoff turning 45/90 into 1/3 slots. */
    uint64_t rb=(b-epoch+tick/2)/tick;
    p.real_slot=epoch+rb*tick;
    if(multiplier<2 || multiplier>4 || a<epoch || b<=a || b-a>250000000ull) return p;
    uint64_t ra=(a-epoch+tick/2)/tick;
    if(rb<=ra+1) return p;
    uint64_t available=rb-ra-1;
    p.count=available<multiplier-1 ? (uint32_t)available : multiplier-1;
    for(uint32_t k=0;k<p.count;k++) {
        uint64_t offset=(uint64_t)(k+1)*(available+1)/(p.count+1);
        p.slots[k]=epoch+(ra+offset)*tick;
        p.phases[k]=(float)(p.slots[k]-a)/(float)(b-a);
    }
    return p;
}
/* Absolute display grid: host wake jitter does not accumulate into a slower
 * refresh clock. A delayed call can use its still-current slot, never two calls
 * on one target slot and never a near-zero catch-up burst. */
static inline uint64_t gsf_grid_deadline(uint64_t epoch,uint64_t desired,uint64_t now,
                                        uint64_t last_target,uint64_t last_call,uint32_t hz)
{
    uint64_t tick=gsf_refresh_period(hz);
    uint64_t due=desired;
    if(last_target && due<last_target+tick) due=last_target+tick;
    if(last_call && due<last_call+tick/2) due=last_call+tick/2;
    if(due<epoch) due=epoch;
    due=epoch+(due-epoch+tick-1)/tick*tick;
    if(now>due+tick/2) {
        uint64_t current=epoch+(now-epoch+tick/2)/tick*tick;
        if(current>due) due=current;
    }
    return due;
}
/* Preserve source progress under output backpressure, before GPU generation.
 * A normal short handoff does not change admission. Already-spent refresh
 * slots cannot be sold a second time as additional interpolation budget. */
static inline uint32_t gsf_budget_outputs(uint32_t requested,uint64_t blocked,uint32_t hz)
{
    uint64_t tick=gsf_refresh_period(hz);
    if(blocked<=tick/4) return requested;
    uint64_t spent=(blocked+tick-1)/tick;
    return spent<requested ? requested-(uint32_t)spent : 0;
}

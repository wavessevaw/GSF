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
/* A late wake/late GPU must never produce a compressed catch-up burst. */
static inline uint64_t gsf_output_deadline(uint64_t desired,uint64_t last,uint32_t hz)
{
    uint64_t next=last ? last+gsf_refresh_period(hz) : 0;
    return desired>next ? desired : next;
}

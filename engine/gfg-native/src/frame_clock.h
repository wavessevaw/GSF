/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GFG_FRAME_CLOCK_H
#define GFG_FRAME_CLOCK_H
#include <stdint.h>
/* Absolute monotonic WSI handoff deadlines, not physical scanout evidence.
 * Real frames occupy refresh slots on a fixed lattice; synthetic frames use
 * only unoccupied slots between consecutive real frames. A 60 Hz source on
 * 90 Hz output can therefore admit one synthetic frame on alternating pairs.
 * Re-anchoring bounds latency to two measured source intervals. */
enum { GFG_MAX_SYNTHETIC = 3, GFG_REAL_OUTPUT = 3, GFG_OUTPUT_COUNT = 4 };
typedef struct {
    uint64_t period_ns, real_ns, resyncs;
    uint64_t source_ns, interval_ns, target_ns[GFG_OUTPUT_COUNT];
    uint64_t previous_real_ns; /* A endpoint of this pair's presentation plan */
    uint64_t grid_origin_ns, grid_period_ns, real_slot;
    unsigned mask, discontinuity, overspeed;
} gfg_frame_clock;
static inline void gfg_frame_clock_pair(gfg_frame_clock *c,uint64_t source,unsigned multiplier) {
    if (!c->period_ns || (multiplier<2 || multiplier>4)) return;
    const uint64_t p=c->period_ns;
    c->previous_real_ns=c->real_ns;
    const uint64_t previous_source=c->source_ns;
    uint64_t dt=previous_source && source>previous_source?source-previous_source:0;
    uint64_t gap=6*p;
    if(gap<250000000ull)gap=250000000ull;
    const int discontinuity=previous_source && (!dt || dt>gap);
    if(!dt || dt>gap)dt=c->interval_ns?c->interval_ns:multiplier*p;
    c->source_ns=source;c->interval_ns=dt;c->mask=0;
    c->discontinuity=discontinuity;
    for(unsigned k=0;k<GFG_MAX_SYNTHETIC;++k)
        c->target_ns[k]=source+(k+1)*dt/multiplier;
    c->target_ns[GFG_REAL_OUTPUT]=source+dt;
    if(discontinuity || (c->grid_period_ns && c->grid_period_ns!=p)) {
        ++c->resyncs;
        c->grid_origin_ns=0;
    }
    c->grid_period_ns=p;
    /* A source faster than output has no spare display slots. Never build
     * unbounded latency by queueing every source frame onto a slower clock.
     * Forward real frames and let the compositor perform any dropping. */
    if(dt<p-p/20) {
        if(!c->overspeed)++c->resyncs;
        c->overspeed=1;c->grid_origin_ns=0;c->real_slot=0;
        c->target_ns[GFG_REAL_OUTPUT]=source;c->real_ns=source;c->discontinuity=1;
        return;
    }
    c->overspeed=0;
    if(!c->grid_origin_ns) {
        c->grid_origin_ns=source+dt;
        c->real_slot=0;c->target_ns[GFG_REAL_OUTPUT]=c->grid_origin_ns;
        c->real_ns=c->grid_origin_ns;
        return; /* First/gap frame establishes history, no synthetic output. */
    }
    const uint64_t desired=source+dt;
    const uint64_t ahead=desired>c->grid_origin_ns?desired-c->grid_origin_ns:0;
    /* Ignore sub-slot source jitter immediately after a lattice boundary.
     * Unconditional ceil turns alternating +/-100us source noise into full
     * extra/missing refresh slots (3/1 rather than 2/2 at 45->90). A small
     * 5% snap window retains the fixed grid; the causality and bounded-delay
     * checks below still reject a real target before its source exists. */
    uint64_t next_slot=ahead/p+(ahead%p>p/20);
    if(next_slot<=c->real_slot)next_slot=c->real_slot+1;
    const uint64_t target=c->grid_origin_ns+next_slot*p;
    /* Source-rate transitions must not produce unbounded pacing waits. */
    if(target>source+2*dt || target<source || target<c->real_ns+p) {
        ++c->resyncs;c->discontinuity=1;
        c->grid_origin_ns=source+dt;c->real_slot=0;
        c->target_ns[GFG_REAL_OUTPUT]=c->real_ns=c->grid_origin_ns;
        return;
    }
    const uint64_t previous_slot=c->real_slot;
    const uint64_t span=next_slot-previous_slot;
    c->real_slot=next_slot;
    c->real_ns=c->target_ns[GFG_REAL_OUTPUT]=target;
    uint64_t last=previous_slot;
    for(unsigned k=0;k<multiplier-1;++k) {
        const uint64_t offset=span*(k+1)/multiplier;
        const uint64_t slot=previous_slot+offset;
        if(!offset || slot<=last || slot>=next_slot)continue;
        const uint64_t t=c->grid_origin_ns+slot*p;
        /* No frame may be presented before both source frames exist. */
        if(t<=source)continue;
        c->target_ns[k]=t;c->mask|=1u<<k;last=slot;
    }
}
static inline uint64_t gfg_frame_clock_target(const gfg_frame_clock *c,unsigned kind) {
    return kind<GFG_OUTPUT_COUNT?c->target_ns[kind]:0; /* G1/G2/G3, real */
}
/* Map the actual reserved display slot onto the adjacent planned real A/B
 * endpoints. Slot label G2 is NOT an interpolation phase: with x3 on 45->90
 * the sole free slot is halfway between A and B, hence tau=1/2. These are
 * presentation-plan coordinates, not a claim about physical scanout times.
 * Reject stale/invalid anchors rather than substituting a nominal fraction. */
static inline int gfg_frame_clock_phase(const gfg_frame_clock *c,unsigned kind,float *tau) {
    if(!tau || kind>=GFG_MAX_SYNTHETIC || !(c->mask&(1u<<kind)) || c->discontinuity ||
       !c->previous_real_ns || c->target_ns[GFG_REAL_OUTPUT]<=c->previous_real_ns ||
       c->target_ns[kind]<=c->previous_real_ns || c->target_ns[kind]>=c->target_ns[GFG_REAL_OUTPUT])return 0;
    const double t=(double)(c->target_ns[kind]-c->previous_real_ns)/
                   (double)(c->target_ns[GFG_REAL_OUTPUT]-c->previous_real_ns);
    *tau=(float)t;
    return *tau>0.0f && *tau<1.0f;
}
static inline int gfg_frame_clock_admit(const gfg_frame_clock *c,unsigned kind,uint64_t now) {
    return kind<GFG_MAX_SYNTHETIC && (c->mask&(1u<<kind)) && now<c->target_ns[kind];
}
/* Recheck immediately before dispatch, after queue/ring/source-copy delays.
 * GOOGLE timed presents need the same early handoff as the readiness wait.
 * This only removes impossible slots; it does not predict GPU completion. */
static inline unsigned gfg_frame_clock_compute_mask(const gfg_frame_clock *c,
                                                   uint64_t now,uint64_t lead_ns) {
    unsigned mask=0;
    for(unsigned k=0;k<GFG_MAX_SYNTHETIC;++k)
        if((c->mask&(1u<<k)) && c->target_ns[k]>lead_ns &&
           now<c->target_ns[k]-lead_ns)mask|=1u<<k;
    return mask;
}
#endif

/* SPDX-License-Identifier: GPL-3.0-or-later
 * Snapshot ring lifetime, not Vulkan API or physical scanout evidence.
 */
#include "../src/owned_snapshot_ring.h"
#include <assert.h>
#include <stdio.h>
#define NS 1000000000ull

static void independent_lifetime(void) {
    gfg_snapshot_ring ring={0};
    assert(gfg_snapshot_ring_init(&ring,3));
    unsigned prev=0;
    assert(gfg_snapshot_reserve(&ring,1,NS,&prev));
    assert(!gfg_snapshot_pin(&ring,prev,1)); /* GPU not yet done */
    assert(gfg_snapshot_copy_ready(&ring,prev,1,NS+1000000u));
    assert(ring.source_retired==1 && ring.real_retired==0);
    assert(gfg_snapshot_pin(&ring,prev,1)); /* hold A across arrival of B */
    assert(gfg_snapshot_real_retired(&ring,prev,1));
    assert(gfg_snapshot_live(&ring)==1);
    uint64_t prev_id=1;
    for(uint64_t frame=2;frame<=12000u;++frame) {
        const uint64_t arrived=NS+frame*(NS/45u);
        unsigned b=0;
        assert(gfg_snapshot_reserve(&ring,frame,arrived,&b));
        assert(b!=prev);
        assert(gfg_snapshot_copy_ready(&ring,b,frame,arrived+1000000u));
        /* The app B image is already releasable, while A and B still
         * live in independent GPU image memory awaiting interpolation. */
        assert(ring.source_retired==frame);
        assert(ring.real_retired==frame-1u);
        assert(gfg_snapshot_pin(&ring,prev,prev_id)); /* GPU reads A */
        assert(gfg_snapshot_pin(&ring,b,frame)); /* GPU reads B */
        assert(gfg_snapshot_pin(&ring,b,frame)); /* next A history */
        assert(gfg_snapshot_real_retired(&ring,b,frame));
        assert(gfg_snapshot_unpin(&ring,prev,prev_id)); /* GPU A retired */
        assert(gfg_snapshot_unpin(&ring,b,frame)); /* GPU B retired */
        assert(gfg_snapshot_unpin(&ring,prev,prev_id)); /* no longer previous */
        assert(gfg_snapshot_live(&ring)==1);
        prev=b;prev_id=frame;
    }
    assert(gfg_snapshot_unpin(&ring,prev,prev_id));
    assert(gfg_snapshot_live(&ring)==0);
    assert(ring.source_retired==12000u && ring.real_retired==12000u);
    assert(!ring.overflows);
    assert(ring.invalid==1); /* deliberately attempted premature GPU pin */
}
static void overload_never_overwrites(void) {
    gfg_snapshot_ring r={0};
    assert(gfg_snapshot_ring_init(&r,3));
    unsigned slots[3];
    for(unsigned i=0;i<3;++i) {
        assert(gfg_snapshot_reserve(&r,i+1,NS+i*10000000ull,&slots[i]));
        assert(gfg_snapshot_copy_ready(&r,slots[i],i+1,NS+i*10000000ull+100u));
    }
    unsigned rejected=0;
    assert(!gfg_snapshot_reserve(&r,4,NS+40000000ull,&rejected));
    assert(r.overflows==1 && gfg_snapshot_live(&r)==3);
    assert(r.source_retired==3 && r.real_retired==0);
    /* Consumer owns the original until REAL output retirement. */
    assert(gfg_snapshot_real_retired(&r,slots[0],1));
    assert(gfg_snapshot_reserve(&r,4,NS+40000000ull,&rejected));
    assert(gfg_snapshot_copy_ready(&r,rejected,4,NS+40000001ull));
    for(unsigned i=1;i<3;++i)
        assert(gfg_snapshot_real_retired(&r,slots[i],i+1));
    assert(gfg_snapshot_real_retired(&r,rejected,4));
    assert(r.real_retired==4 && r.source_retired==4);
    assert(gfg_snapshot_live(&r)==0);
    assert(!gfg_snapshot_ring_init(&r,9));
}
static void pinned_cannot_be_reused(void) {
    gfg_snapshot_ring r={0};unsigned a=0,b=0;
    assert(gfg_snapshot_ring_init(&r,1));
    assert(gfg_snapshot_reserve(&r,10,NS,&a));
    assert(gfg_snapshot_copy_ready(&r,a,10,NS+100));
    assert(gfg_snapshot_pin(&r,a,10));
    assert(gfg_snapshot_real_retired(&r,a,10));
    assert(!gfg_snapshot_reserve(&r,11,NS+200,&b));
    assert(r.overflows==1);
    assert(!gfg_snapshot_unpin(&r,a,9)); /* stale token cannot unpin */
    assert(gfg_snapshot_unpin(&r,a,10));
    assert(gfg_snapshot_reserve(&r,11,NS+200,&b));
    assert(!gfg_snapshot_copy_ready(&r,b,11,NS+199));
    assert(gfg_snapshot_copy_ready(&r,b,11,NS+201));
    assert(!gfg_snapshot_copy_ready(&r,b,11,NS+202));
    assert(gfg_snapshot_real_retired(&r,b,11));
    assert(gfg_snapshot_live(&r)==0);
}
int main(void) {
    independent_lifetime();
    overload_never_overwrites();
    pinned_cannot_be_reused();
    puts("GSF_SNAPSHOT_LIFETIME independent_source_retirement=1"
         " output_pins=1 no_overwrite=1 12000_frames=PASS"
         " physical_scanout=UNVERIFIED");
    return 0;
}

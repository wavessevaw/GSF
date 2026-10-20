/* SPDX-License-Identifier: GPL-3.0-or-later
 * Private GPU snapshot ownership for a future detached post-game output.
 * Synchronization-neutral contract: caller must externally serialize this
 * structure and signal copy_ready only AFTER its actual GPU fence completes.
 * No VkImage/VkSemaphore may be reused just because a token was issued.
 */
#ifndef GFG_OWNED_SNAPSHOT_RING_H
#define GFG_OWNED_SNAPSHOT_RING_H
#include <stdint.h>
#include <string.h>
enum { GFG_SNAPSHOT_MAX_IMAGES=8 };
enum { GFG_SNAPSHOT_FREE=0, GFG_SNAPSHOT_COPYING=1,
       GFG_SNAPSHOT_READY=2 };
typedef struct {
    uint64_t frame_id,source_ns,copied_ns;
    uint32_t pins;
    unsigned state,real_pending;
} gfg_snapshot_image;
typedef struct {
    gfg_snapshot_image images[GFG_SNAPSHOT_MAX_IMAGES];
    unsigned capacity,cursor;
    uint64_t last_frame_id,source_retired,real_retired,overflows,invalid;
} gfg_snapshot_ring;

static inline int gfg_snapshot_ring_init(gfg_snapshot_ring *r,unsigned capacity) {
    if(!r||!capacity||capacity>GFG_SNAPSHOT_MAX_IMAGES)return 0;
    memset(r,0,sizeof(*r));r->capacity=capacity;return 1;
}
static inline int gfg_snapshot_reserve(gfg_snapshot_ring *r,
                       uint64_t frame_id,uint64_t source_ns,unsigned *slot) {
    if(!r||!slot||!frame_id||!source_ns||frame_id<=r->last_frame_id) {
        if(r)++r->invalid;
        return 0;
    }
    for(unsigned n=0;n<r->capacity;++n) {
        const unsigned idx=(r->cursor+n)%r->capacity;
        gfg_snapshot_image *p=&r->images[idx];
        if(p->state!=GFG_SNAPSHOT_FREE)continue;
        *p=(gfg_snapshot_image){.frame_id=frame_id,.source_ns=source_ns,
            .state=GFG_SNAPSHOT_COPYING,.real_pending=1};
        r->last_frame_id=frame_id;r->cursor=(idx+1)%r->capacity;
        *slot=idx;return 1;
    }
    ++r->overflows;return 0; /* backpressure, never overwrite live GPU memory */
}
static inline gfg_snapshot_image *gfg_snapshot_lookup(
        gfg_snapshot_ring *r,unsigned slot,uint64_t frame_id) {
    if(!r||slot>=r->capacity||!frame_id||
       r->images[slot].frame_id!=frame_id||
       r->images[slot].state==GFG_SNAPSHOT_FREE) {
        if(r)++r->invalid;
        return 0;
    }
    return &r->images[slot];
}
/* The virtual game's app VkImage may now be returned to Acquire; its
 * independent private snapshot stays pinned for future GPU/output work. */
static inline int gfg_snapshot_copy_ready(gfg_snapshot_ring *r,
                      unsigned slot,uint64_t frame_id,uint64_t copied_ns) {
    gfg_snapshot_image *p=gfg_snapshot_lookup(r,slot,frame_id);
    if(!p||p->state!=GFG_SNAPSHOT_COPYING||
       copied_ns<p->source_ns) {
        if(r)++r->invalid;
        return 0;
    }
    p->state=GFG_SNAPSHOT_READY;p->copied_ns=copied_ns;
    ++r->source_retired;return 1;
}
static inline int gfg_snapshot_pin(gfg_snapshot_ring *r,
                                   unsigned slot,uint64_t frame_id) {
    gfg_snapshot_image *p=gfg_snapshot_lookup(r,slot,frame_id);
    if(!p||p->state!=GFG_SNAPSHOT_READY||p->pins==UINT32_MAX) {
        if(r)++r->invalid;
        return 0;
    }
    ++p->pins;return 1;
}
static inline void gfg_snapshot_release_if_done(gfg_snapshot_image *p) {
    if(!p->real_pending && !p->pins)
        *p=(gfg_snapshot_image){0};
}
/* The actual downstream WSI (or its correctly observed retirement) owns
 * this transition. App image release MUST NOT wait for this call. */
static inline int gfg_snapshot_real_retired(gfg_snapshot_ring *r,
                                           unsigned slot,uint64_t frame_id) {
    gfg_snapshot_image *p=gfg_snapshot_lookup(r,slot,frame_id);
    if(!p||p->state!=GFG_SNAPSHOT_READY||!p->real_pending) {
        if(r)++r->invalid;
        return 0;
    }
    p->real_pending=0;++r->real_retired;
    gfg_snapshot_release_if_done(p);return 1;
}
/* Release only after the actual optional synthetic GPU/output work retires;
 * cancellation must also wait for any already-submitted GPU reads. */
static inline int gfg_snapshot_unpin(gfg_snapshot_ring *r,
                                     unsigned slot,uint64_t frame_id) {
    gfg_snapshot_image *p=gfg_snapshot_lookup(r,slot,frame_id);
    if(!p||p->state!=GFG_SNAPSHOT_READY||!p->pins) {
        if(r)++r->invalid;
        return 0;
    }
    --p->pins;gfg_snapshot_release_if_done(p);return 1;
}
static inline unsigned gfg_snapshot_live(const gfg_snapshot_ring *r) {
    unsigned n=0;
    if(r)for(unsigned i=0;i<r->capacity;++i)
        n+=r->images[i].state!=GFG_SNAPSHOT_FREE;
    return n;
}
#endif

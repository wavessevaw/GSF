/* SPDX-License-Identifier: GPL-3.0-or-later
 * Vulkan-independent application-image ownership for a future virtual
 * swapchain. A frame can be reacquired once its GPU snapshot is retired,
 * without waiting for the PRIVATE output WSI to present that snapshot.
 * No actual Vulkan layer hooking is activated by this contract.
 */
#ifndef GFG_VIRTUAL_SOURCE_H
#define GFG_VIRTUAL_SOURCE_H
#include <stdint.h>
#include <string.h>
enum { GFG_VIRTUAL_MAX_IMAGES=8 };
typedef enum {
    GFG_VIRTUAL_FREE=0,
    GFG_VIRTUAL_ACQUIRED=1,
    GFG_VIRTUAL_COPYING=2
} gfg_virtual_image_state;
typedef struct {
    unsigned count,cursor,closed;
    gfg_virtual_image_state state[GFG_VIRTUAL_MAX_IMAGES];
    uint64_t token[GFG_VIRTUAL_MAX_IMAGES];
    uint64_t serial,acquired,retired,blocked,invalid;
} gfg_virtual_source;
static inline int gfg_virtual_init(gfg_virtual_source *v,unsigned count) {
    if(!v || !count || count>GFG_VIRTUAL_MAX_IMAGES)return 0;
    memset(v,0,sizeof(*v));v->count=count;
    /* deterministic parity mapping for 2-image test; contract does not
     * require a specific order and can be used with other image counts. */
    v->cursor=count>1?1:0;
    return 1;
}
static inline int gfg_virtual_acquire(gfg_virtual_source *v,unsigned *index) {
    if(!v||!index||!v->count||v->closed)return 0;
    for(unsigned i=0;i<v->count;++i) {
        unsigned idx=(v->cursor+i)%v->count;
        if(v->state[idx]!=GFG_VIRTUAL_FREE)continue;
        v->state[idx]=GFG_VIRTUAL_ACQUIRED;
        v->cursor=(idx+1)%v->count;
        *index=idx;++v->acquired;return 1;
    }
    ++v->blocked;return 0;
}
/* Present is an application-source retirement request, not an output WSI
 * present. The bridge must consume the app's rendering semaphore exactly
 * once and snapshot the image. */
static inline int gfg_virtual_begin_snapshot(gfg_virtual_source *v,
                    unsigned index,uint64_t *ticket) {
    if(!v||!ticket||index>=v->count ||
       v->state[index]!=GFG_VIRTUAL_ACQUIRED) {
        if(v)++v->invalid;
        return 0;
    }
    const uint64_t id=++v->serial;
    v->token[index]=id;v->state[index]=GFG_VIRTUAL_COPYING;
    *ticket=id;return 1;
}
static inline int gfg_virtual_release(gfg_virtual_source *v,
                  unsigned index,uint64_t ticket,int copy_fence_signalled) {
    if(!v||index>=v->count ||
       v->state[index]!=GFG_VIRTUAL_COPYING || !ticket ||
       v->token[index]!=ticket) {
        if(v)++v->invalid;
        return 0;
    }
    if(!copy_fence_signalled)return 0;
    v->state[index]=GFG_VIRTUAL_FREE;
    v->token[index]=0;
    ++v->retired;return 1;
}
static inline int gfg_virtual_cancel_acquire(gfg_virtual_source *v,unsigned index) {
    if(!v||index>=v->count || v->state[index]!=GFG_VIRTUAL_ACQUIRED) {
        if(v) ++v->invalid;
        return 0;
    }
    v->state[index]=GFG_VIRTUAL_FREE;return 1;
}
static inline void gfg_virtual_close(gfg_virtual_source *v) {
    if(v)v->closed=1;
}
static inline int gfg_virtual_all_idle(const gfg_virtual_source *v) {
    if(!v)return 0;
    for(unsigned i=0;i<v->count;++i)
        if(v->state[i]!=GFG_VIRTUAL_FREE)return 0;
    return 1;
}
#endif

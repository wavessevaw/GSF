/* SPDX-License-Identifier: GPL-3.0-or-later
 * Reference contract for ONE output owner. Separate from the source
 * swapchain. Never submit these tickets together with game's native present.
 * This plans output; Vulkan/scanout implementation is a later milestone.
 */
#ifndef GFG_OUTPUT_OWNER_H
#define GFG_OUTPUT_OWNER_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef enum { GFG_OWNER_REAL=1, GFG_OWNER_SYNTHETIC=2 } gfg_owner_kind;
typedef struct {
    gfg_owner_kind kind;
    uint64_t from_id,to_id,due_ns,source_capture_ns;
} gfg_owner_ticket;
typedef struct {
    uint64_t refresh_ns,max_lag_ns,gap_ns;
    uint64_t previous_id,previous_capture_ns,last_real_id,last_due_ns;
    uint64_t real_count,synthetic_count,skipped_count,gap_count,error_count;
    int have_prev,started;
} gfg_output_owner;
static inline uint64_t gfg_owner_max(uint64_t a,uint64_t b) {
    return a>b?a:b;
}
static inline uint64_t gfg_owner_add(uint64_t a,uint64_t b) {
    return UINT64_MAX-a<b?UINT64_MAX:a+b;
}
static inline void gfg_owner_init(gfg_output_owner *o,
                                   uint64_t refresh_ns,uint64_t max_lag_ns) {
    memset(o,0,sizeof(*o));
    o->refresh_ns=refresh_ns;o->max_lag_ns=max_lag_ns;
    o->gap_ns=250000000ull;
}
static inline int gfg_owner_real(gfg_output_owner *o,gfg_owner_ticket *e,
                                  uint64_t id,uint64_t captured,uint64_t due) {
    if((o->last_real_id && id<=o->last_real_id) ||
       (o->last_due_ns && due<o->last_due_ns)) {
        ++o->error_count;return 0;
    }
    *e=(gfg_owner_ticket){.kind=GFG_OWNER_REAL,.from_id=id,.to_id=id,
        .due_ns=due,.source_capture_ns=captured};
    o->last_due_ns=due;o->last_real_id=id;++o->real_count;return 1;
}
/* On capture B, hold the image pair A,B until interpolation is ready.
 * First B schedules A, AB, B. Subsequent C schedules BC, C only.
 * Queue/texture ownership must live until the private output retires them.
 * gpu_ready_ns represents ACTUAL completion, never just GPU submission.
 * Late/invalid synthetic -> real-only, with a bounded real deadline.
 * Tickets are commands for a PRIVATE output surface, not WSI presents.
 */
static inline size_t gfg_owner_capture(gfg_output_owner *o,
        uint64_t id,uint64_t captured_ns,uint64_t gpu_ready_ns,
        int interpolated_valid,gfg_owner_ticket out[3]) {
    if(!o||!out||!o->refresh_ns||!id||!captured_ns||
       (o->have_prev && (id<=o->previous_id||
                         captured_ns<=o->previous_capture_ns))) {
        if(o)++o->error_count;
        return 0;
    }
    if(!o->have_prev) {
        o->previous_id=id;o->previous_capture_ns=captured_ns;
        o->have_prev=1;return 0;
    }
    const uint64_t period=o->refresh_ns;
    const uint64_t dt=captured_ns-o->previous_capture_ns;
    const int gap=dt>gfg_owner_max(o->gap_ns,6*period);
    size_t n=0;
    if(!o->started) {
        uint64_t at=gfg_owner_max(captured_ns,
            o->last_due_ns?gfg_owner_add(o->last_due_ns,period):captured_ns);
        if(!gfg_owner_real(o,&out[n],o->previous_id,o->previous_capture_ns,at))
            return 0;
        ++n;o->started=1;
    }
    const uint64_t earliest=gfg_owner_max(captured_ns,
        gfg_owner_add(o->last_due_ns,period));
    /* The GPU cannot produce an interpolated frame before B is captured.
     * Reserve at least one refresh interval to complete this work. */
    const uint64_t synthetic_due=gfg_owner_max(earliest,
        gfg_owner_add(captured_ns,period));
    const uint64_t real_after_synthetic=gfg_owner_add(synthetic_due,period);
    const int synth_ok=!gap && interpolated_valid && dt>=2*period &&
        gpu_ready_ns>0 && gpu_ready_ns<=synthetic_due &&
        real_after_synthetic>=captured_ns &&
        real_after_synthetic-captured_ns<=o->max_lag_ns;
    if(synth_ok) {
        out[n++]=(gfg_owner_ticket){.kind=GFG_OWNER_SYNTHETIC,
            .from_id=o->previous_id,.to_id=id,
            .due_ns=synthetic_due,.source_capture_ns=captured_ns};
        o->last_due_ns=synthetic_due;++o->synthetic_count;
    } else {
        ++o->skipped_count;
        if(gap)++o->gap_count;
    }
    const uint64_t real_due=synth_ok?real_after_synthetic:earliest;
    if(!gfg_owner_real(o,&out[n],id,captured_ns,real_due))return n;
    ++n;o->previous_id=id;o->previous_capture_ns=captured_ns;
    return n;
}
/* Early quit: even a single source frame must eventually be output. */
static inline size_t gfg_owner_flush(gfg_output_owner *o,
                            uint64_t now_ns,gfg_owner_ticket out[1]) {
    if(!o||!out||!o->have_prev||o->started)return 0;
    if(!gfg_owner_real(o,out,o->previous_id,o->previous_capture_ns,
                       gfg_owner_max(now_ns,o->previous_capture_ns)))return 0;
    o->started=1;return 1;
}
#endif

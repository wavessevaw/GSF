/* SPDX-License-Identifier: GPL-3.0-or-later
 * Immutable pair tickets for an isolated output owner. CPU reference policy,
 * not the live swapchain implementation. All times in one monotonic domain.
 * Deadline never moves after dispatch; late synthetic expires, real survives.
 */
#ifndef GSF_PACING_OWNER_PLAN_H
#define GSF_PACING_OWNER_PLAN_H
#include <stdint.h>
#include <string.h>
#include "frame_clock.h"
enum { GSF_SLOT_RESERVED,GSF_GPU_DISPATCHED,GSF_GPU_READY,GSF_WSI_SUBMITTED,
       GSF_WSI_ACCEPTED,GSF_SLOT_EXPIRED,GSF_FEEDBACK_OBSERVED };
enum { GSF_PC_STABLE,GSF_PC_CONGESTED,GSF_PC_RECOVERY_PROBE,GSF_PC_STABILIZING };
typedef struct {
    uint64_t source_a,source_b,swapchain_generation,sequence;
    uint64_t target_ns,deadline_ns,ready_ns,accepted_ns;
    float tau;
    unsigned slot_id,output_index,state;
} gsf_pacing_ticket;
typedef struct {
    gsf_pacing_ticket outputs[2];
    uint64_t real_target_ns,real_limit_ns,a_target_ns,b_target_ns;
    unsigned count,mask,real_submitted;
} gsf_pacing_plan;
typedef struct {
    unsigned state,successes;
    uint64_t last_probe_ns,recoveries,reentries,probes;
} gsf_pacing_controller;
static inline const char *gsf_pc_name(unsigned state) {
    const char *names[]={"STABLE","CONGESTED","RECOVERY_PROBE","STABILIZING"};
    return state<4?names[state]:"unknown";
}
static inline uint64_t gsf_pc_add(uint64_t a,uint64_t b) {return UINT64_MAX-a<b?UINT64_MAX:a+b;}
/* The latency configuration shifts BOTH endpoint coordinates before tau is
 * computed. The owner must retain A/B source resources until those tickets
 * retire. A direct-WSI caller without such retention may NOT enable buffering.
 */
static inline gsf_pacing_plan gsf_pc_plan(const gfg_frame_clock *clock,
    uint64_t source_a,uint64_t source_b,uint64_t generation,uint64_t sequence,
    uint64_t now,uint64_t buffer_ns,uint64_t start_delay_ns,const uint64_t compute_ns[2],
    uint64_t copy_ns,uint64_t acquire_ns,uint64_t lead_ns,uint64_t uncertainty_ns) {
    gsf_pacing_plan plan={0};
    plan.a_target_ns=gsf_pc_add(clock->previous_real_ns,buffer_ns);
    plan.b_target_ns=plan.real_target_ns=gsf_pc_add(clock->target_ns[2],buffer_ns);
    plan.real_limit_ns=plan.real_target_ns;
    if(clock->discontinuity || plan.a_target_ns>=plan.b_target_ns)return plan;
    uint64_t cost=gsf_pc_add(start_delay_ns,gsf_pc_add(copy_ns,gsf_pc_add(acquire_ns,uncertainty_ns)));
    for(unsigned k=0;k<2;++k)if(clock->mask&(1u<<k)) {
        const uint64_t target=gsf_pc_add(clock->target_ns[k],buffer_ns);
        if(target<=lead_ns || target<=plan.a_target_ns || target>=plan.b_target_ns)continue;
        const uint64_t deadline=target-lead_ns;
        const uint64_t ready=gsf_pc_add(now,gsf_pc_add(cost,compute_ns[plan.count]));
        if(ready>deadline)continue;
        gsf_pacing_ticket *t=&plan.outputs[plan.count];
        *t=(gsf_pacing_ticket){.source_a=source_a,.source_b=source_b,.swapchain_generation=generation,
            .sequence=sequence,.target_ns=target,.deadline_ns=deadline,.slot_id=k,.output_index=plan.count,
            .tau=(float)((double)(target-plan.a_target_ns)/(double)(plan.b_target_ns-plan.a_target_ns))};
        if(!(t->tau>0 && t->tau<1))continue;
        plan.mask|=1u<<k;++plan.count;
    }
    return plan;
}
static inline int gsf_pc_dispatch(gsf_pacing_ticket *t) {
    if(t->state!=GSF_SLOT_RESERVED)return 0;
    t->state=GSF_GPU_DISPATCHED;return 1;
}
static inline int gsf_pc_ready(gsf_pacing_ticket *t,uint64_t now) {
    if(t->state!=GSF_GPU_DISPATCHED)return 0;
    t->ready_ns=now;
    t->state=now<=t->deadline_ns?GSF_GPU_READY:GSF_SLOT_EXPIRED;
    return t->state==GSF_GPU_READY;
}
static inline int gsf_pc_submit(gsf_pacing_ticket *t,uint64_t now) {
    if(t->state!=GSF_GPU_READY)return 0;
    if(now>t->deadline_ns){t->state=GSF_SLOT_EXPIRED;return 0;}
    t->state=GSF_WSI_SUBMITTED;return 1;
}
static inline int gsf_pc_accept(gsf_pacing_ticket *t,uint64_t now) {
    if(t->state!=GSF_WSI_SUBMITTED)return 0;
    t->accepted_ns=now;t->state=GSF_WSI_ACCEPTED;return 1;
}
static inline int gsf_pc_feedback(gsf_pacing_ticket *t) {
    if(t->state!=GSF_WSI_ACCEPTED)return 0;
    t->state=GSF_FEEDBACK_OBSERVED;return 1;
}
static inline int gsf_pc_real_once(gsf_pacing_plan *p) {
    if(p->real_submitted)return 0;
    p->real_submitted=1;return 1;
}
static inline void gsf_pc_result(gsf_pacing_controller *c,int gpu_ready,int wsi_accepted) {
    if(gpu_ready && wsi_accepted) {
        if(++c->successes>=2) {
            if(c->state!=GSF_PC_STABLE)++c->recoveries;
            c->state=GSF_PC_STABLE;
        } else c->state=GSF_PC_STABILIZING;
    } else {
        if(c->state==GSF_PC_STABLE)++c->reentries;
        c->successes=0;c->state=GSF_PC_CONGESTED;
    }
}
static inline int gsf_pc_probe(gsf_pacing_controller *c,uint64_t now,uint64_t interval) {
    uint64_t period=interval>62500000ull?250000000ull:interval*4;
    if(period<50000000ull)period=50000000ull;
    if(c->last_probe_ns && (now<c->last_probe_ns || now-c->last_probe_ns<period))return 0;
    c->last_probe_ns=now;c->state=GSF_PC_RECOVERY_PROBE;++c->probes;return 1;
}
#endif

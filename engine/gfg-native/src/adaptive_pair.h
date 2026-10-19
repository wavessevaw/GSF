/* SPDX-License-Identifier: GPL-3.0-or-later
 * Experimental direct-WSI recovery policy with immutable A/G/B slots.
 * Pure CPU policy: never waits, submits, or changes application Vulkan objects.
 */
#ifndef GSF_ADAPTIVE_PAIR_H
#define GSF_ADAPTIVE_PAIR_H
#include <stdint.h>
#include <limits.h>
enum { GSF_SYNCING,GSF_GENERATING,GSF_DEGRADED,GSF_REACQUIRE };
typedef struct {
    unsigned state,misses,success_streak,probes,eligible,probing;
    uint64_t next_probe_ns,backoff_ns,last_probe_ns;
    uint64_t baseline_g1,baseline_real,period,origin;
    uint64_t target_g1,target_real,latest_g1,latest_real;
    uint64_t observed_ready_ns,sequence;
    uint64_t cooldown_exits,generation_restarts,stable_entries;
} gsf_adaptive_pair;
typedef struct {int admit,probe;uint64_t g1,real;} gsf_pair_plan;
static inline uint64_t gsf_ap_add(uint64_t a,uint64_t b) {
    return UINT64_MAX-a<b?UINT64_MAX:a+b;
}
static inline uint64_t gsf_ap_ceil(uint64_t n,uint64_t origin,uint64_t p) {
    if(!p||n<=origin)return origin;
    uint64_t d=n-origin,k=d/p+(d%p!=0);
    return k>(UINT64_MAX-origin)/p?UINT64_MAX:origin+k*p;
}
static inline void gsf_ap_init(gsf_adaptive_pair *s) {
    *s=(gsf_adaptive_pair){.state=GSF_SYNCING,.backoff_ns=500000000ull};
}
static inline const char *gsf_ap_name(unsigned state) {
    static const char *names[]={"STABILIZING","STABLE","CONGESTED","RECOVERY_PROBE"};
    return state<4?names[state]:"unknown";
}
static inline gsf_pair_plan gsf_ap_begin(gsf_adaptive_pair *s,uint64_t now,
  uint64_t seq,uint64_t origin,uint64_t period,uint64_t g1,uint64_t real,
  uint64_t predicted_ns,int slot,int history,int pressure,int discontinuity) {
    gsf_pair_plan out={0};
    s->sequence=seq;s->eligible=0;s->probing=0;s->observed_ready_ns=0;
    s->baseline_g1=g1;s->baseline_real=real;s->origin=origin;s->period=period;
    if(discontinuity) {
        s->state=GSF_SYNCING;s->misses=0;s->success_streak=0;
    }
    if(!slot||!history||pressure||!origin||!period||!real)return out;
    if(s->state==GSF_DEGRADED) {
        if(now<s->next_probe_ns)return out;
        s->state=GSF_REACQUIRE;
        ++s->probes;++s->cooldown_exits;
    }
    /* The direct WSI path has no independent retained output pipeline.
     * It may NOT extend B or retime a computed G. A buffered owner needs
     * separate resource lifetime proof; see pacing_owner_plan.h. */
    s->latest_real=real;s->latest_g1=g1;
    const uint64_t lead=period/4;
    if(g1<=lead || now>=g1-lead)return out;
    if(gsf_ap_add(now,predicted_ns)>=g1-lead && s->state!=GSF_REACQUIRE) {
        s->state=GSF_DEGRADED;s->next_probe_ns=gsf_ap_add(now,s->backoff_ns);
        return out;
    }
    /* Recovery may test an old forecast, but its readiness and WSI deadline
     * stay exactly the same. A timer never declares generation recovered. */
    s->eligible=1;s->probing=(s->state==GSF_REACQUIRE);
    s->target_g1=g1;s->target_real=real;
    out=(gsf_pair_plan){.admit=1,.probe=(int)s->probing,.g1=g1,.real=real};
    return out;
}
static inline int gsf_ap_ready(gsf_adaptive_pair *s,uint64_t gpu_observed_ns,
  uint64_t *g1,uint64_t *real) {
    if(!s->eligible)return 0;
    s->observed_ready_ns=gpu_observed_ns;
    if(s->target_g1<=s->period/4 || gpu_observed_ns>s->target_g1-s->period/4)return 0;
    *g1=s->target_g1;*real=s->target_real;return 1;
}
static inline void gsf_ap_outcome(gsf_adaptive_pair *s,uint64_t now,int sent,int accepted) {
    if(accepted && sent && s->observed_ready_ns && s->eligible) {
        if(s->state==GSF_REACQUIRE)++s->generation_restarts;
        s->misses=0;++s->success_streak;
        s->backoff_ns=500000000ull;
        if(s->success_streak>=2) {
            if(s->state!=GSF_GENERATING)++s->stable_entries;
            s->state=GSF_GENERATING;
        } else s->state=GSF_SYNCING;
        return;
    }
    if(!sent) {
        if(s->state==GSF_REACQUIRE) {
            s->state=GSF_DEGRADED;s->next_probe_ns=gsf_ap_add(now,s->backoff_ns);
        }
        return;
    }
    s->success_streak=0;
    if(s->misses<5)++s->misses;
    if(s->state==GSF_REACQUIRE || s->misses>=3) {
        s->state=GSF_DEGRADED;s->next_probe_ns=gsf_ap_add(now,s->backoff_ns);
        if(s->backoff_ns<2000000000ull) {
            s->backoff_ns*=2;
            if(s->backoff_ns>2000000000ull)s->backoff_ns=2000000000ull;
        }
    }
}
#endif

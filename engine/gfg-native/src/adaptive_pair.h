/* SPDX-License-Identifier: GPL-3.0-or-later
 * Experimental 0.1.8 bounded real/synthetic pair planner.
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
    static const char *names[]={"SYNCING","GENERATING","DEGRADED","REACQUIRE"};
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
        ++s->probes;
    }
    const uint64_t last=gsf_ap_add(real,2*period); /* 2 refreshes max added B schedule */
    if(last<=period || now>=last-period)return out;
    s->latest_real=last;s->latest_g1=last-period;
    const uint64_t lead=period/4;
    uint64_t candidate=gsf_ap_add(now,gsf_ap_add(predicted_ns,lead));
    if(candidate<g1)candidate=g1;
    uint64_t g=gsf_ap_ceil(candidate,origin,period);
    if(g>s->latest_g1) {
        /* A stale calibrated estimate must not strand the entire session.
         * Probe at most once per backoff, never extend the real-frame limit. */
        if(s->state!=GSF_REACQUIRE) {
            s->state=GSF_DEGRADED;s->next_probe_ns=gsf_ap_add(now,s->backoff_ns);
            return out;
        }
        candidate=gsf_ap_add(now,lead);
        if(candidate<g1)candidate=g1;
        g=gsf_ap_ceil(candidate,origin,period);
    }
    if(g>s->latest_g1)return out;
    uint64_t b=gsf_ap_add(g,period);
    if(b<real)b=real;
    if(b>last)return out;
    s->eligible=1;s->probing=(s->state==GSF_REACQUIRE);
    s->target_g1=g;s->target_real=b;
    out=(gsf_pair_plan){.admit=1,.probe=(int)s->probing,.g1=g,.real=b};
    return out;
}
static inline int gsf_ap_ready(gsf_adaptive_pair *s,uint64_t gpu_observed_ns,
  uint64_t *g1,uint64_t *real) {
    if(!s->eligible)return 0;
    uint64_t candidate=gsf_ap_ceil(gsf_ap_add(gpu_observed_ns,s->period/4),
                                   s->origin,s->period);
    if(candidate<s->target_g1)candidate=s->target_g1;
    if(candidate>s->latest_g1)return 0;
    uint64_t b=gsf_ap_add(candidate,s->period);
    if(b<s->baseline_real)b=s->baseline_real;
    if(b>s->latest_real)return 0;
    s->target_g1=candidate;s->target_real=b;
    s->observed_ready_ns=gpu_observed_ns;
    *g1=candidate;*real=b;return 1;
}
static inline void gsf_ap_outcome(gsf_adaptive_pair *s,uint64_t now,int sent,int accepted) {
    if(accepted) {
        s->misses=0;++s->success_streak;
        s->backoff_ns=500000000ull;
        if(s->success_streak>=2)s->state=GSF_GENERATING;
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

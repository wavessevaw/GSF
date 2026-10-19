/* SPDX-License-Identifier: GPL-3.0-or-later
 * Admission arithmetic only: no waits, Vulkan calls or source-FPS threshold.
 * Submit-to-GPU-start includes dependencies/delivery, not pure queue wait.
 */
#ifndef GSF_GENERATION_BUDGET_H
#define GSF_GENERATION_BUDGET_H
#include <stdint.h>

#define GSF_GENERATION_RETRY_NS 2000000000ull
#define GSF_GENERATION_MARGIN_NS 500000ull
/* First GPU timestamps may include cold pipeline/shader execution on lavapipe
 * or a new RADV shader cache. Never use one outlier to block all recovery for
 * two seconds. Permit only bounded early remeasurement of the first sample. */
#define GSF_GENERATION_INITIAL_SPIKE_NS 50000000ull
#define GSF_GENERATION_COLD_PROBE_NS 250000000ull

typedef struct {
    uint64_t submit_delay_ns;
    int calibrated;
} gsf_generation_budget;

static inline void gsf_generation_budget_observe(gsf_generation_budget *b,
                                                int valid,uint64_t delay_ns) {
    /* An invalid observation cannot erase the last trustworthy cost and
     * reopen a burst. Bounded retries provide recovery from an old estimate. */
    if(valid && delay_ns<=GSF_GENERATION_RETRY_NS) {
        b->submit_delay_ns=delay_ns;b->calibrated=1;
    }
}

static inline uint64_t gsf_generation_budget_cost(const gsf_generation_budget *b,
                                                  uint64_t gpu_span_ns) {
    uint64_t delay=b->calibrated?b->submit_delay_ns:0;
    return gpu_span_ns>UINT64_MAX-delay?UINT64_MAX:gpu_span_ns+delay;
}

static inline int gsf_generation_budget_fits(const gsf_generation_budget *b,
    uint64_t gpu_span_ns,uint64_t target_ns,uint64_t lead_ns,uint64_t now) {
    if(target_ns<=lead_ns || target_ns-lead_ns<=now)return 0;
    uint64_t slack=target_ns-lead_ns-now;
    uint64_t cost=gsf_generation_budget_cost(b,gpu_span_ns);
    return slack>GSF_GENERATION_MARGIN_NS && cost<slack-GSF_GENERATION_MARGIN_NS;
}

static inline int gsf_generation_retry_due(uint64_t last_dispatch_ns,uint64_t now) {
    return now>=last_dispatch_ns && now-last_dispatch_ns>=GSF_GENERATION_RETRY_NS;
}

static inline int gsf_generation_cold_retry_due(uint64_t last_dispatch_ns,
    uint64_t now, uint32_t timed_samples, uint64_t first_gpu_span_ns) {
    return timed_samples == 1 &&
           first_gpu_span_ns > GSF_GENERATION_INITIAL_SPIKE_NS &&
           now >= last_dispatch_ns &&
           now-last_dispatch_ns >= GSF_GENERATION_COLD_PROBE_NS;
}

static inline int gsf_generation_probe_allowed(const gsf_generation_budget *b,
    uint64_t last_dispatch_ns,uint64_t now) {
    return !b->calibrated || gsf_generation_retry_due(last_dispatch_ns,now);
}
#endif

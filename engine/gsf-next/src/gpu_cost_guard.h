/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional timestamp-driven synthetic GPU admission: cheap x2 first,
 * cautious two-output x3 only with measured headroom.
 * Real frames and Output Owner source retirement are never gated.
 * This is an independent original implementation, not LSFG-VK code.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

struct gsf_gpu_cost_guard {
    uint64_t last_sample;
    uint64_t denied;
    uint64_t probes;
    uint32_t cooldown;
    uint32_t good_samples;
    uint32_t last_synthetic_us;
    uint32_t last_outputs;
    bool sample_seen;
    bool probe_pending;
    bool x3_confirmed;
};

/* A probe is released ONLY by completed positive-output Vulkan timestamps.
 * Duplicate, real-only and zero-cost samples cannot promote x3.
 * If a two-output probe breaches the budget, x3 is immediately revoked.
 */
static inline void gsf_gpu_cost_observe(struct gsf_gpu_cost_guard *g,
                                        uint64_t sample_id,uint32_t synthetic_outputs,
                                        uint32_t measured_pair_us,uint32_t budget_us)
{
    if(!g||!budget_us||!sample_id||sample_id<=g->last_sample||
       synthetic_outputs<1u||synthetic_outputs>2u||!measured_pair_us)
        return;
    g->last_sample=sample_id;
    g->last_outputs=synthetic_outputs;
    g->sample_seen=true;
    g->probe_pending=false;
    g->last_synthetic_us=measured_pair_us;
    if(measured_pair_us>budget_us) {
        g->cooldown=7u;
        g->good_samples=0u;
        g->x3_confirmed=false;
    } else {
        g->cooldown=0u;
        if(g->good_samples<2u)g->good_samples++;
        /* Real measured two-output x3 under budget, not a multiplied
         * benchmark fps or a queue-depth-based prediction. */
        if(synthetic_outputs==2u)g->x3_confirmed=true;
    }
}

/* Each positive admission is a new probe. Do not issue another synthetic
 * GPU job until its completed Vulkan timing is observed. That bounds work
 * even under driver stalls and incomplete timestamps.
 *
 * For x3, first establish two affordable x2 samples. Only attempt two
 * outputs if a conservative 2x extrapolation of the measured *total*
 * x2 pair cost fits the budget. After a verified affordable x3 result,
 * maintain two outputs until the first measured overrun.
 *
 * Normal x2/x3 calls only; any requested x4 is capped to two synthetic
 * outputs while budget control is active.
 */
static inline uint32_t gsf_gpu_cost_admit(struct gsf_gpu_cost_guard *g,
                                          uint32_t requested,uint32_t budget_us)
{
    if(!g||!budget_us)return requested;
    if(!requested)return 0u;
    if(g->cooldown) {
        g->cooldown--;
        g->denied+=requested;
        return 0u;
    }
    if(g->probe_pending) {
        g->denied+=requested;
        return 0u;
    }
    uint32_t allowed=1u;
    if(requested>=2u&&g->good_samples>=2u) {
        bool predicted=(uint64_t)g->last_synthetic_us*2u<=(uint64_t)budget_us;
        if(g->x3_confirmed||predicted)allowed=2u;
    }
    g->probe_pending=true;
    ++g->probes;
    g->denied+=requested-allowed;
    return allowed;
}

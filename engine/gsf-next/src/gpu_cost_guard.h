/* SPDX-License-Identifier: GPL-3.0-or-later
 * Experimental GPU-time budget for Alpha synthetic admission.
 * Written for GSF; no third-party shader / frame generation source.
 *
 * Only actual completed Vulkan timestamp samples train the gate. The game
 * source and Output Owner real-frame transport are not affected.
 * 0 = disabled, preserving existing behaviour. When enabled, the guard
 * initially probes ONE synthetic and blocks further work until its GPU
 * timestamp is observed; expensive probes impose a 7-frame cooldown.
 * It never invents an x3/x4 throughput multiplier in saturated scenes.
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
    bool sample_seen;
    bool probe_pending;
};

static inline void gsf_gpu_cost_observe(struct gsf_gpu_cost_guard *g,
                                        uint64_t sample_id,uint32_t synthetic_outputs,
                                        uint32_t measured_pair_us,uint32_t budget_us)
{
    if(!g||!budget_us||!sample_id||sample_id<=g->last_sample||
       !synthetic_outputs||synthetic_outputs>3u||!measured_pair_us)
        return;
    g->last_sample=sample_id;
    g->sample_seen=true;
    g->probe_pending=false;
    g->last_synthetic_us=measured_pair_us;
    if(measured_pair_us>budget_us) {
        g->cooldown=7u;
        g->good_samples=0u;
    } else {
        g->cooldown=0u;
        if(g->good_samples<2u)g->good_samples++;
    }
}

/* Call exactly once per source pair after reading newly completed timestamp.
 * The result is NEVER >1: budget mode targets inexpensive x2 before x3/x4.
 * No fresh GPU timestamps => one probe only, avoiding unbounded work.
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
    if(!g->sample_seen||g->good_samples<2u) {
        g->probe_pending=true;
        g->probes++;
        g->denied+=requested-1u;
        return 1u;
    }
    if(requested>1u)g->denied+=requested-1u;
    return 1u;
}

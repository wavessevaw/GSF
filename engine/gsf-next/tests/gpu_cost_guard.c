/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_cost_guard.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct gsf_gpu_cost_guard g={0};
    const uint32_t budget=1500u;
    assert(gsf_gpu_cost_admit(&g,3u,0u)==3u);
    assert(!g.probe_pending && g.denied==0u);
    assert(gsf_gpu_cost_admit(&g,3u,budget)==1u);
    assert(g.probe_pending);
    for(int k=0;k<500;k++)
        assert(gsf_gpu_cost_admit(&g,3u,budget)==0u);
    assert(g.probes==1u);
    /* Wrong/zero/real-only samples must not release a GPU probe token. */
    gsf_gpu_cost_observe(&g,0u,1u,500u,budget);
    gsf_gpu_cost_observe(&g,1u,0u,500u,budget);
    assert(g.probe_pending);
    gsf_gpu_cost_observe(&g,1u,1u,3500u,budget);
    assert(!g.probe_pending&&g.cooldown==7u&&g.last_synthetic_us==3500u);
    gsf_gpu_cost_observe(&g,1u,1u,100u,budget);
    assert(g.cooldown==7u);
    for(int k=0;k<7;k++)
        assert(gsf_gpu_cost_admit(&g,3u,budget)==0u);
    assert(gsf_gpu_cost_admit(&g,3u,budget)==1u);
    assert(g.probe_pending);
    gsf_gpu_cost_observe(&g,2u,1u,1200u,budget);
    assert(g.good_samples==1u);
    assert(gsf_gpu_cost_admit(&g,3u,budget)==1u && g.probe_pending);
    gsf_gpu_cost_observe(&g,3u,1u,900u,budget);
    assert(g.good_samples==2u);
    for(int k=0;k<1000;k++)
        assert(gsf_gpu_cost_admit(&g,3u,budget)==1u);
    gsf_gpu_cost_observe(&g,4u,1u,1800u,budget);
    assert(g.good_samples==0u && g.cooldown==7u);
    for(int k=0;k<7;k++)
        assert(gsf_gpu_cost_admit(&g,2u,budget)==0u);
    assert(gsf_gpu_cost_admit(&g,2u,budget)==1u);
    assert(gsf_gpu_cost_admit(&g,0u,budget)==0u);
    assert(gsf_gpu_cost_admit(&g,1u,budget)==0u); /* probe still pending */
    puts("GSF_GPU_COST_GUARD opt_in=1 no_unbounded_probes=1"
         " one_synthetic_max=1 feedback_hysteresis=1 gpu_cooldown=1"
         " no_assumed_output_fps=1 PASS");
    return 0;
}

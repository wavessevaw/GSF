/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_cost_guard.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct gsf_gpu_cost_guard g={0};
    const uint32_t budget=3000u;
    assert(gsf_gpu_cost_admit(&g,3u,0u)==3u);
    assert(!g.probe_pending && g.denied==0u);

    /* x2 is first, regardless of nominal ×3 and without GPU samples. */
    assert(gsf_gpu_cost_admit(&g,2u,budget)==1u);
    assert(g.probe_pending);
    for(int k=0;k<500;k++)
        assert(gsf_gpu_cost_admit(&g,2u,budget)==0u);
    assert(g.probes==1u);

    /* Real-only or bad observations never release an in-flight probe. */
    gsf_gpu_cost_observe(&g,0u,1u,500u,budget);
    gsf_gpu_cost_observe(&g,1u,0u,500u,budget);
    gsf_gpu_cost_observe(&g,1u,3u,500u,budget);
    assert(g.probe_pending);
    gsf_gpu_cost_observe(&g,1u,1u,7000u,budget);
    assert(!g.probe_pending&&g.cooldown==7u&&g.last_synthetic_us==7000u);
    gsf_gpu_cost_observe(&g,1u,1u,200u,budget);
    assert(g.cooldown==7u);
    for(int k=0;k<7;k++)
        assert(gsf_gpu_cost_admit(&g,2u,budget)==0u);

    /* First two affordable x2 probes; the second observed pair has
     * enough headroom to conservatively trial x3 (2 * 1100 <= 3000). */
    assert(gsf_gpu_cost_admit(&g,2u,budget)==1u);
    gsf_gpu_cost_observe(&g,2u,1u,1200u,budget);
    assert(g.good_samples==1u);
    assert(gsf_gpu_cost_admit(&g,2u,budget)==1u);
    gsf_gpu_cost_observe(&g,3u,1u,1100u,budget);
    assert(g.good_samples==2u&&!g.x3_confirmed);
    assert(gsf_gpu_cost_admit(&g,1u,budget)==1u);
    gsf_gpu_cost_observe(&g,4u,1u,1100u,budget);
    assert(gsf_gpu_cost_admit(&g,2u,budget)==2u && g.probe_pending);
    assert(gsf_gpu_cost_admit(&g,2u,budget)==0u); /* no double-probe */
    gsf_gpu_cost_observe(&g,5u,2u,2000u,budget);
    assert(g.x3_confirmed && g.last_outputs==2u);
    assert(gsf_gpu_cost_admit(&g,2u,budget)==2u);
    gsf_gpu_cost_observe(&g,6u,2u,2700u,budget);
    assert(gsf_gpu_cost_admit(&g,3u,budget)==2u); /* x4 never admitted */
    assert(g.denied>0u);

    /* An actual overbudget x3 immediately cancels confirmed promotion,
     * invokes cooldown, and restarts from cheap x2 probes. */
    gsf_gpu_cost_observe(&g,7u,2u,3500u,budget);
    assert(!g.x3_confirmed&&g.good_samples==0u&&g.cooldown==7u);
    for(int k=0;k<7;k++)
        assert(gsf_gpu_cost_admit(&g,2u,budget)==0u);
    assert(gsf_gpu_cost_admit(&g,2u,budget)==1u);
    assert(gsf_gpu_cost_admit(&g,0u,budget)==0u);
    assert(gsf_gpu_cost_admit(&g,1u,budget)==0u);
    assert(g.probe_pending);

    /* x3 never promoted with heavy x2 even after 2 good samples. */
    struct gsf_gpu_cost_guard heavy={0};
    assert(gsf_gpu_cost_admit(&heavy,2u,budget)==1u);
    gsf_gpu_cost_observe(&heavy,1u,1u,2300u,budget);
    assert(gsf_gpu_cost_admit(&heavy,2u,budget)==1u);
    gsf_gpu_cost_observe(&heavy,2u,1u,2200u,budget);
    assert(heavy.good_samples==2u);
    assert(gsf_gpu_cost_admit(&heavy,2u,budget)==1u);

    puts("GSF_GPU_COST_GUARD real_priority=1 no_unbounded_probes=1"
         " affordable_x2_first=1 measured_x3_promotion=1"
         " x3_overrun_revoked=1 gpu_cooldown=1 cap_x3=1"
         " no_assumed_output_fps=1 PASS");
    return 0;
}

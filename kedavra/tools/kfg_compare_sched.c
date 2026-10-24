// SPDX-License-Identifier: GPL-3.0-or-later
// Controlled completion trace, not a measurement of GPU scheduling or throughput.
#include "kfg_sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#ifndef KFG_COMPARE_CANDIDATE
#define KFG_COMPARE_CANDIDATE 0
#endif
#if KFG_COMPARE_CANDIDATE
#include "kfg_runtime.h"
#endif

int main(void)
{
    puts("multiplier,completion_delay_vblanks,generated,premature_generated,repeats,real,degraded_transitions");
    for (int mult = 2; mult <= 3; ++mult)
        for (int delay = 1; delay <= 7; ++delay) {
            kfg_sched s;
            kfg_sched_config cfg = kfg_sched_default_config((unsigned)mult);
            kfg_sched_init(&s, &cfg);
            int generated = 0, premature = 0, repeats = 0, real = 0;
            for (int v = 0; v < 12000; ++v) {
                if (v % mult == 0) {
                    uint64_t id = (uint64_t)(v / mult + 1);
                    kfg_sched_on_latch(&s, id, v);
#if !KFG_COMPARE_CANDIDATE
                    // Actual legacy Gamescope bridge: on_me(latch+2), before submission.
                    kfg_sched_on_me(&s, id, v + 2, true, false);
#endif
                }
#if KFG_COMPARE_CANDIDATE
                if (v >= delay && (v - delay) % mult == 0) {
                    uint64_t id = (uint64_t)((v - delay) / mult + 1);
                    if (kfg_runtime_completed(1, 1, id, id))
                        kfg_sched_on_me(&s, id, v, true, false);
                }
#endif
                kfg_decision d = kfg_sched_decide(&s, v);
                if (d.show == KFG_SHOW_GEN) {
                    ++generated;
                    if (v < (int)(d.src_b - 1) * mult + delay) ++premature;
                }
                repeats += d.show == KFG_SHOW_REPEAT;
                real += d.show == KFG_SHOW_REAL;
            }
#if KFG_COMPARE_CANDIDATE
            assert(premature == 0);
#endif
            printf("%d,%d,%d,%d,%d,%d,%llu\n", mult, delay, generated, premature, repeats, real,
                (unsigned long long)s.n_degrade);
        }
    return 0;
}

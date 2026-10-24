// SPDX-License-Identifier: GPL-3.0-or-later
#include "kfg_runtime.h"
#include "kfg_sched.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(!kfg_runtime_completed(1, 2, 8, 9)); // stale history epoch
    assert(!kfg_runtime_completed(2, 2, 0, 9)); // unsubmitted work
    assert(!kfg_runtime_completed(2, 2, 10, 9));
    assert(kfg_runtime_completed(2, 2, 10, 10));
    assert(kfg_runtime_admit(100, 1000, 200, 300, 100));
    assert(!kfg_runtime_admit(1000, 1000, 0, 0, 0));
    assert(!kfg_runtime_admit(1001, 1000, 0, 0, 0));
    assert(!kfg_runtime_admit(100, 1000, 200, 600, 100)); // exact cutoff is late
    assert(!kfg_runtime_admit(100, 1000, UINT64_MAX, 1, 1));
    assert(!kfg_runtime_admit(100, 1000, 1, UINT64_MAX, 1));
    assert(!kfg_runtime_slot_free(true, 4, 100)); // visible/queued/GPU/consumer ref
    assert(!kfg_runtime_slot_free(false, 101, 100)); // GPU writer still pending
    assert(kfg_runtime_slot_free(false, 100, 100));

    // x2/x3: randomized delayed completion, resets, absent completions and rollover.
    for (unsigned mult = 2; mult <= 3; ++mult) {
        kfg_sched s;
        kfg_sched_config cfg = kfg_sched_default_config(mult);
        kfg_sched_init(&s, &cfg);
        uint64_t epoch = 1, seed = 7;
        struct { uint64_t id, epoch, seq; int ready; } jobs[32] = {0};
        int submitted = 0;
        for (int v = 0; v < 20000; ++v) {
            if (v % 257 == 0) { ++epoch; kfg_sched_reset_history(&s); }
            if (v % (int)mult == 0) {
                seed = seed * 6364136223846793005ull + 1;
                uint64_t id = (uint64_t)v + 1;
                kfg_sched_on_latch(&s, id, v);
                unsigned slot = (unsigned)submitted++ % 32;
                jobs[slot].id = id; jobs[slot].epoch = epoch;
                jobs[slot].seq = id; jobs[slot].ready = v + 1 + (int)(seed % 7);
            }
            for (unsigned j = 0; j < 32; ++j)
                if (jobs[j].seq && jobs[j].ready <= v &&
                    kfg_runtime_completed(jobs[j].epoch, epoch, jobs[j].seq, jobs[j].seq))
                {
                    kfg_sched_on_me(&s, jobs[j].id, v, true, false);
                    jobs[j].seq = 0;
                }
            kfg_decision d = kfg_sched_decide(&s, v);
            if (d.show == KFG_SHOW_GEN) {
                bool completed = false;
                for (unsigned j = 0; j < 32; ++j)
                    if (jobs[j].id == d.src_b && jobs[j].epoch == epoch && jobs[j].ready <= v) completed = true;
                assert(completed);
            }
        }
    }
    puts("PASS: runtime admission, retirement, epoch and x2/x3 completion stress (40000 vblanks)");
    return 0;
}

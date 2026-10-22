// SPDX-License-Identifier: GPL-3.0-or-later
// Integration-contract counterexample using the actual production scheduler.
// No GPU: completion occurs at latch+5 in this deliberately overloaded model.
#include "kfg_sched.h"
#include <assert.h>
#include <stdio.h>

static int run(int actual_completion) {
    kfg_sched s;
    kfg_sched_config cfg=kfg_sched_default_config(3);
    kfg_sched_init(&s,&cfg);
    int premature=0, generated=0;
    for(int k=0;k<360;k++) {
        if(k%3==0) {
            uint64_t id=(uint64_t)(k/3+1);
            kfg_sched_on_latch(&s,id,k);
            // Production bridge currently declares success at latch+2, before submitting.
            if(!actual_completion) kfg_sched_on_me(&s,id,k+2,true,false);
        }
        // Proposed adapter must call this only from observed completion, with pair identity.
        if(actual_completion && k>=5 && (k-5)%3==0)
            kfg_sched_on_me(&s,(uint64_t)((k-5)/3+1),k,true,false);
        kfg_decision d=kfg_sched_decide(&s,k);
        if(d.show==KFG_SHOW_GEN) {
            generated++;
            int done=(int)(d.src_b-1)*3+5;
            if(k<done) premature++;
        }
    }
    printf("%s: generated=%d before-modeled-completion=%d degraded=%llu\n",
           actual_completion?"completion-observed":"fixed-plus-two", generated,premature,
           (unsigned long long)s.n_degrade);
    if(actual_completion) assert(s.n_degrade>0);
    return premature;
}
int main(void) {
    assert(run(0)>0);
    assert(run(1)==0);
    puts("PASS: production-scheduler completion contract counterexample");
    return 0;
}

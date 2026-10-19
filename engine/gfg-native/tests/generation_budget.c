/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/generation_budget.h"
#include <assert.h>
#include <stdio.h>

int main(int argc,char **argv) {
    gsf_generation_budget b={0};
    const uint64_t now=10000000000ull;
    /* A 7ms shader span fits 12ms slack. The observed 35.7ms before GPU
     * start makes the same job impossible, independent of source FPS. */
    assert(gsf_generation_budget_fits(&b,7000000,now+12000000,0,now));
    gsf_generation_budget_observe(&b,1,35700000);
    assert(gsf_generation_budget_cost(&b,7000000)==42700000);
    assert(!gsf_generation_budget_fits(&b,7000000,now+12000000,0,now));
    assert(!gsf_generation_probe_allowed(&b,now,now+100000000));
    assert(!gsf_generation_probe_allowed(&b,now,now+GSF_GENERATION_RETRY_NS-1));
    assert(gsf_generation_probe_allowed(&b,now,now+GSF_GENERATION_RETRY_NS));
    assert(!gsf_generation_retry_due(now,now-1));
    gsf_generation_budget_observe(&b,0,0);
    assert(b.calibrated && b.submit_delay_ns==35700000);
    gsf_generation_budget_observe(&b,1,UINT64_MAX);
    assert(b.submit_delay_ns==35700000);
    assert(gsf_generation_budget_cost(&b,UINT64_MAX)==UINT64_MAX);
    assert(!gsf_generation_budget_fits(&b,1,now-1,0,now));
    assert(!gsf_generation_budget_fits(&b,1,5,10,0));
    assert(!gsf_generation_budget_fits(&b,UINT64_MAX,UINT64_MAX,0,0));
    /* Recovery observation reopens normal admission immediately. */
    gsf_generation_budget_observe(&b,1,40000);
    assert(gsf_generation_budget_fits(&b,7000000,now+12000000,0,now));
    gsf_generation_budget_observe(&b,1,0);
    assert(b.calibrated && b.submit_delay_ns==0);
    if(argc>1) {
        FILE *f=fopen(argv[1],"r");assert(f);
        char line[256];assert(fgets(line,sizeof(line),f));
        unsigned rows=0,span_only=0,full_cost=0,impossible=0;
        unsigned long long delay,span,slack;
        while(fgets(line,sizeof(line),f)) {
            assert(sscanf(line,"%llu,%llu,%llu",&delay,&span,&slack)==3);
            gsf_generation_budget old={0},measured={0};
            gsf_generation_budget_observe(&measured,1,delay);
            int a=gsf_generation_budget_fits(&old,span,slack,0,0);
            int z=gsf_generation_budget_fits(&measured,span,slack,0,0);
            assert(!z || a);
            span_only+=(unsigned)a;full_cost+=(unsigned)z;
            impossible+=(unsigned)(a && !z);++rows;
        }
        fclose(f);
        assert(rows==1409 && impossible>900 && full_cost>100);
        printf("Calibrated Deck deadline feasibility: %u samples, span-only=%u full-cost=%u impossible-span-only=%u (not a performance prediction)\n",
            rows,span_only,full_cost,impossible);
    }
    puts("generation-budget: delayed start, recovery, invalid sample, expired slot, overflow PASS");
    return 0;
}

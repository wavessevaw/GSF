/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/generation_budget.h"
#include "../src/frame_clock.h"
#include <assert.h>
#include <stdio.h>

int main(int argc,char **argv) {
    unsigned streak=0;
    assert(gsf_generation_quality(2,2,8000000,11111111,&streak)==1);
    for(unsigned i=0;i<239;++i)assert(gsf_generation_quality(1,2,1000000,11111111,&streak)==1);
    assert(gsf_generation_quality(1,2,1000000,11111111,&streak)==2);
    for(unsigned i=0;i<500;++i)assert(gsf_generation_quality(1,1,1000000,11111111,&streak)==1);
    assert(gsf_generation_quality(2,2,4000000,11111111,&streak)==2 && !streak);
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
    /* The initial cold 206ms sample must not suppress 45->90 for 2s.
     * The early retry is bounded to one outstanding slow sample; a second
     * measured slow sample restores the normal two-second retry cadence. */
    assert(!gsf_generation_cold_retry_due(now,now+249999999ull,1,206705060));
    assert(gsf_generation_cold_retry_due(now,now+250000000ull,1,206705060));
    assert(!gsf_generation_cold_retry_due(now,now+250000000ull,2,206705060));
    assert(!gsf_generation_cold_retry_due(now,now+250000000ull,1,10000000));
    assert(!gsf_generation_cold_retry_due(now,now-1,1,206705060));
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
    if(argc>2) {
        FILE *f=fopen(argv[2],"r");assert(f);
        char line[256];assert(fgets(line,sizeof line,f));
        unsigned rows=0,fit[3]={0};
        unsigned mode;unsigned long long dt,p,delay,span;long long slack;
        while(fgets(line,sizeof line,f)) {
            assert(sscanf(line,"%u,%llu,%llu,%llu,%llu,%lld",&mode,&dt,&p,&delay,&span,&slack)==6);
            assert(mode==2 || mode==3);
            gsf_generation_budget measured={0};gsf_generation_budget_observe(&measured,1,delay);
            unsigned previous_fit=0;
            for(unsigned depth=0;depth<=2;++depth) {
                uint64_t last=0;
                gfg_frame_clock clock={.period_ns=p,.interval_ns=dt,.mask=1,
                    .target_ns={now+p/4+slack,0,0,now+2*dt}};
                gfg_frame_clock_buffer(&clock,depth,&last);
                unsigned affordable=gsf_generation_budget_fits(&measured,span,clock.target_ns[0],p/4,now);
                assert(!previous_fit || affordable);
                fit[depth]+=affordable;previous_fit=affordable;
            }
            ++rows;
        }
        fclose(f);assert(rows==256 && fit[2]>fit[0]);
        printf("Deck dispatched-sample replay (selection biased; not FPS): rows=%u fit0=%u fit1=%u fit2=%u\n",rows,fit[0],fit[1],fit[2]);
    }
    puts("generation-budget: delayed start, recovery, invalid sample, expired slot, overflow PASS");
    return 0;
}

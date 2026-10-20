/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/post_deadline_budget.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define NS 1000000000ull
int main(void) {
    const uint64_t tick=(NS+89u)/90u;
    gfg_post_deadline_budget p={.refresh_tick_ns=tick,
        .max_real_age_ns=80u*1000000u,
        .real_present_cost_ns=1000000u,
        .synthetic_cost_ns=7000000u};
    const uint64_t capture=NS;
    assert(gfg_post_admit_companions(&p,capture,capture+2000000u,0,1)==1);
    assert(gfg_post_admit_companions(&p,capture,capture+2000000u,0,2)==2);
    assert(gfg_post_admit_companions(&p,capture,capture+2000000u,0,3)==3);
    assert(gfg_post_admit_companions(&p,capture,capture+2000000u,0,8)==3);
    assert(gfg_post_admit_companions(&p,capture,capture+70000000u,0,3)==0);
    assert(gfg_post_admit_companions(&p,capture,capture+25000000u,30000000u,3)==1);
    assert(gfg_post_admit_companions(&p,capture,capture+25000000u,45000000u,3)==0);
    assert(gfg_post_admit_companions(&p,capture-1,capture-2,0,2)==0);
    assert(gfg_post_admit_companions(&p,0,capture,0,3)==0);
    for(uint32_t mult=2;mult<=4;mult++) {
        uint32_t previous=3;
        for(uint64_t age=0;age<=100000000ull;age+=1000000ull) {
            const uint32_t count=gfg_post_admit_companions(
                &p,capture,capture+age,0,mult-1u);
            assert(count<=previous && count<=mult-1u);
            previous=count;
            const uint32_t congested=gfg_post_admit_companions(
                &p,capture,capture+age,15000000u,mult-1u);
            assert(congested<=count);
            gfg_post_deadline_budget expensive=p;
            expensive.synthetic_cost_ns=28000000ull;
            assert(gfg_post_admit_companions(
                &expensive,capture,capture+age,0,mult-1u)<=count);
        }
    }
    /* Ten-minute mathematical trace at 45/30/22.5 source to 90 display.
     * No artificial queue-length gain when pending transport debt rises. */
    const uint64_t source_period[]={NS/45u,NS/30u,4*NS/90u};
    for(uint32_t mode=0;mode<3;mode++) {
        const uint32_t mul=mode+2u;
        uint64_t now=NS,admitted=0,denied=0;
        for(uint64_t frame=0;frame<10000u;frame++) {
            now+=source_period[mode];
            const uint64_t debt=(frame%31u)*2500000u;
            const uint32_t result=gfg_post_admit_companions(
                &p,now,now+2000000ull,debt,mul-1u);
            assert(result<=mul-1u);
            if(!result)denied++;else admitted+=result;
            if(debt>=80000000ull)assert(result==0u);
        }
        assert(admitted>0 && denied>0);
        printf("GSF_POST_BUDGET source_mode=%u multiplier=%u"
               " admitted=%llu overbudget=%llu real_mandatory=1 PASS\n",
            mode,mul,(unsigned long long)admitted,
            (unsigned long long)denied);
    }
    puts("GSF_POST_DEADLINE_BUDGET bounded and load-monotone PASS");
    return 0;
}

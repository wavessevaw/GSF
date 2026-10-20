/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "output_plan.h"
#include <assert.h>
#include <stdio.h>

static void pair(uint64_t period, uint32_t hz, uint32_t mult, uint32_t count)
{
    struct gsf_output_plan p = gsf_output_plan(1000000000ull,1000000000ull+period,1000000000ull,hz,mult);
    assert(p.count == count);
    for (uint32_t i = 0; i < p.count; i++) {
        assert(p.phases[i] > 0.0f && p.phases[i] < 1.0f);
        if (i) { assert(p.phases[i] > p.phases[i-1]); assert(p.slots[i] > p.slots[i-1]); }
    }
}
int main(void)
{
    pair(22222222,90,2,1); pair(33333333,90,3,2); pair(44444444,90,4,3);
    pair(33333333,90,4,2); pair(22222222,90,3,1); pair(22222222,90,4,1);
    pair(33333333,60,2,1); pair(50000000,60,3,2); pair(66666666,60,4,3);
    pair(25000000,120,3,2); pair(33333333,120,4,3);
    pair(5000000,90,4,0); pair(500000000,90,4,0);
    for (uint32_t hz=30; hz<=360; hz++)
        for (uint32_t mult=2; mult<=4; mult++)
            for (uint64_t a=0; a<1000000000; a+=1234567) {
                struct gsf_output_plan p=gsf_output_plan(a,a+23456789,0,hz,mult);
                assert(p.count<=mult-1);
                for (uint32_t k=0; k<p.count;k++) {
                    assert(p.phases[k]>0 && p.phases[k]<1);
                    struct gsf_output_plan previous=gsf_output_plan(0,a,0,hz,mult);
                    assert(p.slots[k]>previous.real_slot && p.slots[k]<p.real_slot);
                    if(k) assert(p.slots[k]>p.slots[k-1]);
                }
            }
    /* Fractional source rates must reserve each real endpoint exactly once.
     * The old interior-ticks-only plan generated ~136 total presents at 46/90 x3. */
    for(uint32_t mult=2;mult<=4;mult++) for(uint32_t rate=15;rate<=89;rate++) {
        uint64_t a=1000000000ull,total=0,epoch=a;
        for(uint32_t n=1;n<=rate*60;n++) {
            uint64_t b=epoch+(uint64_t)n*1000000000ull/rate;
            struct gsf_output_plan p=gsf_output_plan(a,b,epoch,90,mult);
            total+=1+p.count;a=b;
        }
        assert(total<=90*60);
    }
    /* Ready jitter, long overload, refresh changes: never compress late output. */
    uint64_t last=1000000000ull;
    for(uint32_t k=0;k<10000;k++) {
        uint32_t hz=k<5000?90:60;
        uint64_t desired=1000000000ull+(uint64_t)k*1000000ull;
        uint64_t next=gsf_output_deadline(desired,last,hz);
        assert(next-last>=gsf_refresh_period(hz));
        last=next+(k%91==0?45000000ull:0);
    }
    puts("display-slot planner: PASS (multipliers, caps, drift, hitches, refresh changes)");
}

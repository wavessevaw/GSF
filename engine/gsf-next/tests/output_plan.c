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
                    assert(p.slots[k]>a && p.slots[k]<a+23456789);
                    if(k) assert(p.slots[k]>p.slots[k-1]);
                }
            }
    puts("display-slot planner: PASS (multipliers, caps, drift, hitches, refresh changes)");
}

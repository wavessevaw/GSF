/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/shadow_admission.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    gsf_shadow_history h={0};
    gsf_shadow_decision d=gsf_shadow_predict(&h,1000000000ull,1);
    assert(!strcmp(d.prediction,"unknown")&&!strcmp(d.reason,"no_history"));
    gsf_shadow_record(&h,1,990000000ull,200000ull,995000000ull);
    d=gsf_shadow_predict(&h,1000000000ull,1);
    assert(!strcmp(d.prediction,"admit") && d.age_ns==5000000ull && d.last_sequence==1);
    gsf_shadow_record(&h,2,1010000000ull,40000000ull,1010000001ull);
    d=gsf_shadow_predict(&h,1010000000ull,1);
    assert(d.last_sequence==1); /* no lookahead */
    d=gsf_shadow_predict(&h,1010000002ull,1);
    assert(!strcmp(d.prediction,"skip") && d.last_sequence==2);
    gsf_shadow_record(&h,3,1020000000ull,30000000ull,1020000001ull);
    gsf_shadow_record(&h,4,1030000000ull,200000ull,1030000001ull);
    d=gsf_shadow_predict(&h,1030000002ull,1);
    assert(d.median3_valid && d.median3_ns==30000000ull && !strcmp(d.prediction,"admit"));
    d=gsf_shadow_predict(&h,2000000000ull,1);
    assert(!strcmp(d.prediction,"unknown") && !strcmp(d.reason,"stale_measurement"));
    for(int i=5;i<=9;++i)gsf_shadow_record(&h,(uint64_t)i,3000000000ull+i,40000000ull,3000000000ull+i);
    d=gsf_shadow_predict(&h,3000000100ull,1);
    assert(d.median5_valid && d.median5_ns==40000000ull);
    int count=0;
    for(int i=0;i<48;++i){d=gsf_shadow_predict(&h,3000000100ull+10000000ull*i,1);if(d.would_probe)++count;}
    assert(count==1);
    d=gsf_shadow_predict(&h,3800000000ull,0);
    assert(!strcmp(d.reason,"not_candidate"));
    puts("shadow_admission PASS: causality, stale, median, hypothetical backoff");
    return 0;
}

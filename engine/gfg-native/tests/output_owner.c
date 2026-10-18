/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/output_owner.h"
#include <assert.h>
#include <stdio.h>
#define NS 1000000000ull
typedef struct {
    gfg_output_owner o;
    uint64_t real_last,real_count,synth_count,last_due;
} run;
static void audit(run *r,const gfg_owner_ticket *e,size_t n) {
    for(size_t i=0;i<n;++i) {
        assert(e[i].due_ns>=r->last_due);
        if(e[i].kind==GFG_OWNER_REAL) {
            assert(e[i].from_id==e[i].to_id);
            assert(e[i].from_id==r->real_last+1);
            r->real_last=e[i].from_id;++r->real_count;
        } else {
            assert(e[i].kind==GFG_OWNER_SYNTHETIC);
            assert(e[i].from_id==r->real_last);
            assert(e[i].to_id==r->real_last+1);
            ++r->synth_count;
        }
        r->last_due=e[i].due_ns;
    }
}
static void ordered_30_to_60(void) {
    run r={0};gfg_owner_init(&r.o,NS/60,NS/20);
    uint64_t t=NS;
    for(uint64_t id=1;id<=240;++id) {
        gfg_owner_ticket e[3];
        size_t n=gfg_owner_capture(&r.o,id,t,t+NS/150,1,e);
        audit(&r,e,n);t+=NS/30;
    }
    assert(r.real_count==240 && r.synth_count==239 && !r.o.error_count);
    printf("owned-output 30->60: real=%llu intermediate=%llu ORDERED PASS\n",
        (unsigned long long)r.real_count,(unsigned long long)r.synth_count);
}
static void gap_recovery_27_to_38(void) {
    run r={0};gfg_owner_init(&r.o,NS/90,NS/20);
    uint64_t t=NS,id=0;
    for(int i=0;i<140;++i) {
        gfg_owner_ticket e[3];++id;
        audit(&r,e,gfg_owner_capture(&r.o,id,t,t+NS/250,1,e));
        t+=NS/27;
    }
    uint64_t before=r.synth_count;
    t+=836000000ull;
    {gfg_owner_ticket e[3];++id;
     size_t n=gfg_owner_capture(&r.o,id,t,t+NS/250,1,e);
     audit(&r,e,n);assert(n==1 && e[0].kind==GFG_OWNER_REAL);}
    for(int i=0;i<240;++i) {
        t+=NS/38;++id;gfg_owner_ticket e[3];
        audit(&r,e,gfg_owner_capture(&r.o,id,t,t+NS/250,1,e));
    }
    assert(r.real_count==id && before>=130);
    assert(r.synth_count-before>=235 && r.o.gap_count==1);
    assert(!r.o.error_count);
    printf("owned-output 27->38 + 836ms gap: real=%llu synth=%llu RECOVERED PASS\n",
       (unsigned long long)r.real_count,(unsigned long long)r.synth_count);
}
static void misses_keep_reals(void) {
    run r={0};gfg_owner_init(&r.o,NS/90,NS/20);
    uint64_t t=NS;
    for(uint64_t id=1;id<=120;++id) {
        uint64_t ready=(id%3)?t+NS/250:t+NS/5;
        gfg_owner_ticket e[3];
        audit(&r,e,gfg_owner_capture(&r.o,id,t,ready,id%7!=0,e));
        t+=NS/30;
    }
    assert(r.real_count==120 && r.synth_count && r.o.skipped_count);
    printf("owned-output misses/cuts: real=%llu skipped=%llu NO REAL LOSS PASS\n",
      (unsigned long long)r.real_count,(unsigned long long)r.o.skipped_count);
}
static void overspeed_no_synth(void) {
    run r={0};gfg_owner_init(&r.o,NS/90,NS/20);
    uint64_t t=NS;
    for(uint64_t id=1;id<=120;++id) {
        gfg_owner_ticket e[3];
        audit(&r,e,gfg_owner_capture(&r.o,id,t,t+NS/250,1,e));
        t+=NS/60;
    }
    assert(r.real_count==120 && r.synth_count==0);
    printf("owned-output 60->90: real=%llu NO STARVATION PASS\n",
         (unsigned long long)r.real_count);
}
static void one_frame_flush(void) {
    run r={0};gfg_owner_init(&r.o,NS/90,NS/20);
    gfg_owner_ticket e[3];
    assert(!gfg_owner_capture(&r.o,1,NS,NS+NS/250,1,e));
    assert(gfg_owner_flush(&r.o,NS+NS/10,e)==1);
    audit(&r,e,1);assert(r.real_count==1);
    assert(gfg_owner_flush(&r.o,NS+NS/10,e)==0);
}
int main(void) {
    ordered_30_to_60();gap_recovery_27_to_38();
    misses_keep_reals();overspeed_no_synth();one_frame_flush();
    puts("owned-output reference PASS. PRIVATE WSI + OLED UNVERIFIED.");
    return 0;
}

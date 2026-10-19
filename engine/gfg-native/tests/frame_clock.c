/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/frame_clock.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>
static const uint64_t ns=1000000000ull;
static unsigned replay(unsigned fps,unsigned mul,unsigned n) {
    const uint64_t p=ns/90,origin=1000000000ull;
    gfg_frame_clock c={.period_ns=p};unsigned output=0;
    for(unsigned i=0;i<n;i++) {
        const uint64_t src=origin+(uint64_t)i*ns/fps;
        gfg_frame_clock_pair(&c,src,mul);
        assert(c.real_ns>=src && c.real_ns<=src+2*c.interval_ns);
        if(i && !c.discontinuity) {
            for(unsigned k=0;k<mul-1;k++)if(c.mask&(1u<<k)) {
                assert(c.target_ns[k]>=src && c.target_ns[k]<c.real_ns);
                assert(gfg_frame_clock_admit(&c,k,c.target_ns[k]-1));
                assert(!gfg_frame_clock_admit(&c,k,c.target_ns[k]));
                ++output;
            }
        }
    }
    printf("%u->90Hz x%u: %u admitted / %u real (resync=%llu)\n",
           fps,mul,output,n,(unsigned long long)c.resyncs);
    return output;
}
static void jittered_integer_ratio(unsigned output_hz,unsigned multiplier) {
    const uint64_t p=ns/output_hz,origin=10*ns;
    gfg_frame_clock c={.period_ns=p};uint64_t last_real=0;
    for(unsigned i=0;i<600;i++) {
        /* Absolute source scheduling alternates tiny overshoot/undershoot.
         * Plain ceil previously produced 3-slot / 1-slot real spacing at 2x,
         * deleting every second synthetic output despite negligible jitter. */
        const uint64_t source=origin+(uint64_t)i*multiplier*p+(i%2?100000:0);
        gfg_frame_clock_pair(&c,source,multiplier);
        assert(c.real_ns>=source && c.real_ns<=source+2*c.interval_ns);
        if(i) {
            assert(c.mask==(1u<<(multiplier-1))-1u);
            assert(c.real_ns-last_real==multiplier*p);
            for(unsigned k=0;k<multiplier-1;k++) {
                assert(c.target_ns[k]>source && c.target_ns[k]<c.real_ns);
                assert(c.real_ns-c.target_ns[k]==(multiplier-1-k)*p);
            }
        }
        last_real=c.real_ns;
    }
}

static void dispatch_deadlines(void) {
    gfg_frame_clock c={.mask=3,.target_ns={100,200,0,300}};
    assert(gfg_frame_clock_compute_mask(&c,89,10)==3);
    assert(gfg_frame_clock_compute_mask(&c,90,10)==2);
    assert(gfg_frame_clock_compute_mask(&c,189,10)==2);
    assert(gfg_frame_clock_compute_mask(&c,190,10)==0);
    assert(gfg_frame_clock_compute_mask(&c,100,0)==2);
    assert(gfg_frame_clock_compute_mask(&c,200,0)==0);
    assert(gfg_frame_clock_compute_mask(&c,0,201)==0);
    assert(c.mask==3 && c.target_ns[GFG_REAL_OUTPUT]==300); /* real/planned targets unchanged */
    c.mask=0;assert(!gfg_frame_clock_compute_mask(&c,0,0));
}
static void hardware_trace(const char *path) {
    FILE *f=fopen(path,"r");assert(f);
    char line[256];unsigned rows=0,old_compute=0,rejected=0,accepted=0;
    while(fgets(line,sizeof line,f)) {
        if(line[0]=='#')continue;
        int64_t slack;unsigned planned,computed,shown;
        assert(sscanf(line,"%" SCNd64 ",%u,%u,%u",&slack,&planned,&computed,&shown)==4);
        /* The old trace lacks a pre-dispatch timestamp. Worker start is a
         * conservative lower bound: rejection here proves it was already
         * too late, but cannot count expirations later during source submit. */
        const uint64_t origin=10*ns,lead=11110617/4;
        assert(slack>-(int64_t)origin && slack<(int64_t)origin);
        gfg_frame_clock c={.mask=planned,.target_ns={origin+lead+slack,0,0,origin+ns}};
        const unsigned mask=gfg_frame_clock_compute_mask(&c,origin,lead);
        assert((mask&~planned)==0 && c.target_ns[GFG_REAL_OUTPUT]==origin+ns);
        if(shown){assert(computed==1 && mask==1);++accepted;}
        if(computed){++old_compute;if(!(mask&1))++rejected;}
        ++rows;
    }
    assert(!ferror(f));fclose(f);
    assert(rows==7546 && old_compute==2474 && accepted==162);
    assert(rejected==303);
    printf("AC Origins replay: %u pairs, %u old dispatches, >=%u already expired at worker start; all %u accepted G1 retained\n",
           rows,old_compute,rejected,accepted);
}
int main(int argc,char **argv) {
    dispatch_deadlines();
    if(argc==2)hardware_trace(argv[1]);
    jittered_integer_ratio(10,2);
    jittered_integer_ratio(90,2);
    jittered_integer_ratio(90,3);
    jittered_integer_ratio(90,4); // 22.5 -> 90, exactly three vacant display slots
    jittered_integer_ratio(60,4);
    jittered_integer_ratio(120,4);
    assert(replay(30,4,600)>=1180); // 30*4 capped at 90: only two slots
    const unsigned four45=replay(45,4,600);
    assert(four45>=590 && four45<=599); // 45*4 capped at 90: only one slot
    const unsigned four60=replay(60,4,600);
    assert(four60>=290 && four60<=310);
    assert(replay(90,4,600)==0);
    /* 27/42 -> 54/84: enough slots for one generator output per real frame. */
    assert(replay(27,2,600)>=590);
    assert(replay(42,2,600)>=590);
    assert(replay(40,2,600)>=590);
    assert(replay(45,2,600)>=590);
    /* 60 -> 90: every other source pair has an available synthetic slot.
     * Do NOT aim for 120 on a 90 Hz compositor. */
    const unsigned x2_60=replay(60,2,600);
    assert(x2_60>=290 && x2_60<=310);
    /* 30->90 x3: two synthetic slots per real. 60->90 x3 is limited. */
    assert(replay(30,3,600)>=1180);
    /* On an output-limited 90Hz lattice, x3 must NOT blindly output 2 frames
     * after a 60FPS source. It can only fill the one free slot per 2 real. */
    const unsigned y2_60=replay(60,2,600);
    const unsigned y3_60=replay(60,3,600);
    assert(y2_60>=290 && y2_60<=310);
    assert(y3_60>=290 && y3_60<=310);
    /* 75 native FPS leaves ~15 slots/s. Both modes fill, not overproduce. */
    const unsigned y2_75=replay(75,2,600);
    const unsigned y3_75=replay(75,3,600);
    assert(y2_75>=105 && y2_75<=135);
    assert(y3_75>=105 && y3_75<=135);
    const unsigned x3_60=replay(60,3,600);
    assert(x3_60>=290 && x3_60<=310);
    assert(replay(90,2,600)==0);
    assert(replay(90,3,600)==0);
    assert(replay(120,2,600)==0);
    /* Changing source and display refresh may re-anchor but cannot build
     * unbounded handoff delay or silently claim generated scanout. */
    gfg_frame_clock c={.period_ns=ns/90};uint64_t t=ns;
    for(unsigned i=0;i<400;i++) {
        uint64_t dt=i<100?ns/27:i<200?ns/60:i<300?ns/42:ns/27;
        t+=dt;gfg_frame_clock_pair(&c,t,2);
        assert(c.real_ns>=t && c.real_ns<=t+2*c.interval_ns);
    }
    c.period_ns=ns/60;t+=ns/27;gfg_frame_clock_pair(&c,t,2);
    assert(!c.mask && c.grid_period_ns==ns/60);
    t+=ns;gfg_frame_clock_pair(&c,t,2);
    assert(c.discontinuity && !c.mask);
    t+=ns/27;gfg_frame_clock_pair(&c,t,2);assert(c.real_ns>=t);
    gfg_frame_clock none={0};gfg_frame_clock_pair(&none,t,3);assert(!none.real_ns);
    /* In-game 2x is a multiplier, not a locked source FPS. Sweep through
     * 30..45 with jitter, cross the 45-FPS display budget, then recover. */
    gfg_frame_clock game={.period_ns=ns/90};t=10*ns;unsigned admitted=0;
    for(unsigned i=0;i<3000;++i) {
        unsigned fps=i<2000?30+(i/25)%16:i<2500?60:40;
        t+=ns/fps+(i%2?100000:0);
        gfg_frame_clock_pair(&game,t,2);
        assert(game.real_ns>=t && game.real_ns<=t+2*game.interval_ns);
        if(game.mask) {
            assert(game.mask==1 && game.target_ns[0]>t && game.target_ns[0]<game.real_ns);
            assert(game.real_ns-game.target_ns[0]>=game.period_ns);
            ++admitted;
        }
    }
    assert(admitted>2600 && admitted<2800);
    puts("frame-clock: bounded 27/42/60/90Hz slots, x2/x3, rate transitions PASS");
}

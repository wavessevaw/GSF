/* SPDX-License-Identifier: GPL-3.0-or-later
 * Discrete event source/GPU/WSI simulation, not a physical FPS prediction.
 * GPU serialization and immutable output tickets are exercised over 600s of
 * each scenario; real outputs survive rejected/late synthetic work.
 */
#include "../src/pacing_owner_plan.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>
typedef struct {uint64_t until;unsigned held;} image_slot;
int main(void) {
    const unsigned fps_values[]={30,45,60,75,90},jitters[]={0,200000,1000000,3000000};
    const uint64_t delays[]={100000,7000000,15000000,35000000,50000000};
    const uint64_t costs[]={5000000,7000000,10000000,15000000};
    uint64_t cases=0,total_real=0,total_generated=0,recovered=0,late_cancel=0;
    for(unsigned output=60;output<=90;output+=30)for(unsigned mul=2;mul<=4;++mul)
    for(unsigned j=0;j<4;++j)for(unsigned d=0;d<5;++d)for(unsigned g=0;g<4;++g)
    for(unsigned buffer=0;buffer<=4;buffer+=4) {
        gfg_frame_clock clock={.period_ns=1000000000ull/output};
        gsf_pacing_controller controller={.state=GSF_PC_CONGESTED};
        uint64_t now=10000000000ull,end=now+600000000000ull,gpu_free=0,last_wsi_target=0;
        image_slot pool[4]={0};uint64_t source=0,real=0,accepted=0;
        while(now<end) {
            unsigned f=fps_values[(source/300)%5];
            uint64_t dt=1000000000ull/f;
            now+=source%2?dt+jitters[j]:dt-jitters[j];++source;
            if(source%5000==0)now+=300000000ull; /* menu/pause -> fresh history */
            gfg_frame_clock_pair(&clock,now,mul);
            uint64_t compute[3]={costs[g],2*costs[g],3*costs[g]};
            const int heavy=(source/120)%2;
            uint64_t queue=heavy?delays[d]:100000;
            gsf_pacing_plan plan=gsf_pc_plan(&clock,source-1,source,1,source,now,
                buffer*clock.period_ns,queue,compute,200000,100000,clock.period_ns/4,10000);
            unsigned reserved=0;
            for(unsigned k=0;k<4;++k)if(pool[k].held && pool[k].until<=now)pool[k].held=0;
            for(unsigned out=0;out<plan.count;++out) {
                unsigned image=0;while(image<4 && pool[image].held)++image;
                if(image==4)break; /* explicit capacity, never overwrite */
                pool[image].held=1;++reserved;
                gsf_pacing_ticket *t=&plan.outputs[out];
                const gsf_pacing_ticket before=*t;
                assert(gsf_pc_dispatch(t));assert(!gsf_pc_dispatch(t));
                uint64_t start=now+queue;if(gpu_free>start)start=gpu_free;
                gpu_free=start+costs[g];
                /* Force an occasional unexpected GPU completion spike. */
                uint64_t done=gpu_free+(source%257==0?80000000:0);
                gpu_free=done;pool[image].until=done;
                if(!gsf_pc_ready(t,done)){++late_cancel;gsf_pc_result(&controller,0,0);continue;}
                if(!gsf_pc_submit(t,done+300000)){++late_cancel;gsf_pc_result(&controller,0,0);continue;}
                assert(!gsf_pc_submit(t,done+300000));
                assert(gsf_pc_accept(t,done+300000));assert(gsf_pc_feedback(t));
                pool[image].until=t->target_ns+clock.period_ns; /* retained through simulated retirement */
                assert(t->tau==before.tau && t->target_ns==before.target_ns && t->deadline_ns==before.deadline_ns);
                assert(done+300000<=t->deadline_ns); /* NO late synthetic present */
                assert(!last_wsi_target || t->target_ns>last_wsi_target);
                last_wsi_target=t->target_ns;++accepted;
                gsf_pc_result(&controller,1,1);
            }
            (void)reserved;
            assert(gsf_pc_real_once(&plan));assert(!gsf_pc_real_once(&plan));++real;
            if(clock.discontinuity)last_wsi_target=0;
            else last_wsi_target=plan.real_target_ns;
            if(!plan.count && clock.mask && gsf_pc_probe(&controller,now,dt)) {
                /* A probe tests observations. Time alone is not success. */
                assert(controller.state==GSF_PC_RECOVERY_PROBE);
                /* No success is fabricated from a timer or scene oracle. */
            }
        }
        assert(real==source);total_real+=real;total_generated+=accepted;recovered+=controller.recoveries;++cases;
    }
    /* Sufficient-headroom acceptance, separately from overload robustness.
     * Every reserved output is submitted once; capacity at source90 is zero.
     * These clocks are simulated; no assertion about a physical 90Hz panel. */
    const unsigned gold_fps[]={30,45,60,60,90,90},gold_mul[]={3,2,2,3,2,3};
    for(unsigned test=0;test<6;++test) {
        gfg_frame_clock gold={.period_ns=11111111};
        uint64_t now=1000000000ull,frames=0,outputs=0,planned=0;
        const uint64_t dt=1000000000ull/gold_fps[test],cost[3]={5000000,10000000,15000000};
        while(now<601000000000ull) {
            now+=dt;++frames;gfg_frame_clock_pair(&gold,now,gold_mul[test]);
            gsf_pacing_plan plan=gsf_pc_plan(&gold,frames-1,frames,1,frames,now,0,
                100000,cost,200000,100000,gold.period_ns/4,10000);
            planned+=(unsigned)__builtin_popcount(gold.mask);
            outputs+=plan.count;
            for(unsigned i=0;i<plan.count;++i) {
                assert(gsf_pc_dispatch(&plan.outputs[i]));
                assert(gsf_pc_ready(&plan.outputs[i],now+100000+cost[i]));
                assert(gsf_pc_submit(&plan.outputs[i],now+400000+cost[i]));
                assert(gsf_pc_accept(&plan.outputs[i],now+400000+cost[i]));
            }
        }
        if(test<2)assert(outputs==(frames-1)*(gold_mul[test]-1));
        if(test==2 || test==3)assert(outputs<=planned && planned<=frames);
        if(test>=4)assert(outputs==0 && planned==0);
        printf("PACING_HEADROOM_PASS source_hz=%u output_hz=90 multiplier=%u seconds=600 real=%"PRIu64" planned=%"PRIu64" accepted=%"PRIu64" deadline_miss=0 physical_scanout=UNVERIFIED\n",
            gold_fps[test],gold_mul[test],frames,planned,outputs);
    }
    /* Explicit impossibility and controlled buffer: one 45FPS pair with
     * 41ms delivery +7ms compute cannot meet an 8ms baseline handoff window. */
    gfg_frame_clock c={.period_ns=11111111};uint64_t src=1000000000ull;
    gfg_frame_clock_pair(&c,src,2);src+=22222222;gfg_frame_clock_pair(&c,src,2);
    uint64_t compute[3]={7000000,14000000,21000000};
    gsf_pacing_plan a=gsf_pc_plan(&c,1,2,1,1,src,0,41000000,compute,200000,100000,c.period_ns/4,10000);
    gsf_pacing_plan b=gsf_pc_plan(&c,1,2,1,1,src,4*c.period_ns,41000000,compute,200000,100000,c.period_ns/4,10000);
    assert(!a.count && b.count==1 && b.outputs[0].tau==0.5f);
    assert(b.real_target_ns-a.real_target_ns==4*c.period_ns);
    assert(recovered>0 && late_cancel>0);
    printf("PACING_OWNER_MODEL_PASS cases=%"PRIu64" seconds_per_case=600 real=%"PRIu64" generated=%"PRIu64" recoveries=%"PRIu64" unexpected_late_cancel=%"PRIu64" max_owned_images=4 immutable_deadlines=1 physical_scanout=UNVERIFIED\n",
        cases,total_real,total_generated,recovered,late_cancel);
    return 0;
}

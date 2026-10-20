/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "renderer_choice.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void train(struct gsf_renderer_choice *s,uint32_t mask,const double costs[3])
{
    for(uint32_t i=0;i<GSF_RENDER_SAMPLES*3+3;i++) {
        enum gsf_renderer method=gsf_renderer_pick(s,mask);
        gsf_renderer_observe(s,s->epoch,method,costs[method]);
    }
    assert(s->settled);
}
int main(void)
{
    for(uint32_t mask=3;mask<=7;mask+=4) {
        struct gsf_renderer_choice schedule={0};
        uint32_t slots[3][3]={{0}},parities[3][2]={{0}};
        for(uint32_t i=0;i<36;i++) {
            enum gsf_renderer m=gsf_renderer_pick(&schedule,mask);
            slots[m][i%3]++;parities[m][i%2]++;
        }
        for(uint32_t m=0;m<3;m++) if(mask & (1u<<m)) {
            for(uint32_t slot=0;slot<3;slot++) assert(slots[m][slot]>0);
            for(uint32_t parity=0;parity<2;parity++) assert(parities[m][parity]>0);
        }
    }
    const uint32_t all=7,one=3;
    struct gsf_renderer_choice copy={0},direct={0},batch={0},noise={0},counts[3]={0};
    train(&copy,all,(double[]){.5,1,1.5});assert(copy.winner==GSF_RENDER_COPY);
    train(&direct,all,(double[]){1,.5,.8});assert(direct.winner==GSF_RENDER_DIRECT);
    train(&batch,all,(double[]){1,.9,.4});assert(batch.winner==GSF_RENDER_BATCH);
    train(&noise,all,(double[]){1,.96,.95});assert(noise.winner==GSF_RENDER_COPY);
    train(&counts[0],one,(double[]){1,.4,0});assert(counts[0].winner==GSF_RENDER_DIRECT);
    train(&counts[1],all,(double[]){1,.9,.4});assert(counts[1].winner==GSF_RENDER_BATCH);
    assert(!counts[2].settled); /* output cardinalities never share incomparable costs */
    uint32_t epoch=batch.epoch;
    for(uint32_t i=0;i<150;i++) gsf_renderer_observe(&batch,epoch,GSF_RENDER_BATCH,i==50 ? 50:.4);
    assert(batch.epoch==epoch && batch.settled); /* isolated hitch */
    for(uint32_t i=0;i<8;i++) gsf_renderer_observe(&batch,epoch,GSF_RENDER_BATCH,2);
    assert(batch.epoch!=epoch && !batch.settled);
    gsf_renderer_observe(&batch,epoch,GSF_RENDER_BATCH,.01);
    assert(batch.samples[GSF_RENDER_BATCH]==0); /* queued results from the old epoch */
    train(&batch,all,(double[]){.3,1,1.1});assert(batch.winner==GSF_RENDER_COPY);
    struct gsf_renderer_choice invalid={0};gsf_renderer_pick(&invalid,all);
    for(uint32_t i=0;i<100;i++) {
        gsf_renderer_observe(&invalid,invalid.epoch,GSF_RENDER_COPY,NAN);
        gsf_renderer_observe(&invalid,invalid.epoch,GSF_RENDER_COPY,INFINITY);
        gsf_renderer_observe(&invalid,invalid.epoch,GSF_RENDER_COPY,-1);
    }
    assert(!invalid.settled && invalid.samples[0]==0);
    for(uint32_t i=0;i<200;i++) (void)gsf_renderer_pick(&invalid,all);
    assert(gsf_renderer_pick(&invalid,all)==GSF_RENDER_COPY && invalid.issued==128);
    /* A single outlier per candidate must not defeat a clear winner. */
    struct gsf_renderer_choice outlier={0};
    for(uint32_t i=0;i<24;i++) {
        enum gsf_renderer m=gsf_renderer_pick(&outlier,all);
        double cost=m==GSF_RENDER_BATCH ? .3:1;
        if(i<3) cost=100;
        gsf_renderer_observe(&outlier,outlier.epoch,m,cost);
    }
    assert(outlier.settled && outlier.winner==GSF_RENDER_BATCH);
    /* Removing an unavailable implementation cannot return an unsupported route. */
    for(uint32_t i=0;i<16;i++) assert(gsf_renderer_pick(&outlier,one)!=GSF_RENDER_BATCH);
    puts("Measured renderer selection: PASS");
}

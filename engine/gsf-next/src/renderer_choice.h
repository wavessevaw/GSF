/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum gsf_renderer { GSF_RENDER_AUTO=-1, GSF_RENDER_COPY, GSF_RENDER_DIRECT, GSF_RENDER_BATCH, GSF_RENDER_COUNT };
#define GSF_RENDER_SAMPLES 8u
struct gsf_renderer_choice {
    double costs[GSF_RENDER_COUNT][GSF_RENDER_SAMPLES];
    uint32_t samples[GSF_RENDER_COUNT], position[GSF_RENDER_COUNT];
    uint32_t epoch, issued, available, locked_samples, shock;
    enum gsf_renderer winner;
    double baseline;
    bool settled;
};

static inline enum gsf_renderer gsf_renderer_requested(void)
{
    const char *name=getenv("GSF_NEXT_RENDERER");
    if(name!=NULL) {
        if(strcmp(name,"copy")==0) return GSF_RENDER_COPY;
        if(strcmp(name,"direct")==0) return GSF_RENDER_DIRECT;
        if(strcmp(name,"batch")==0) return GSF_RENDER_BATCH;
        return GSF_RENDER_AUTO;
    }
    const char *copy=getenv("GSF_NEXT_OUTPUT_COPY"),*batch=getenv("GSF_NEXT_WARP_BATCH");
    if(copy!=NULL && strcmp(copy,"1")==0) return GSF_RENDER_COPY;
    if(batch!=NULL && strcmp(batch,"1")==0) return GSF_RENDER_BATCH;
    if((copy!=NULL && strcmp(copy,"0")==0) || (batch!=NULL && strcmp(batch,"0")==0)) return GSF_RENDER_DIRECT;
    return GSF_RENDER_AUTO;
}

static inline const char *gsf_renderer_name(enum gsf_renderer mode)
{
    switch(mode) {
    case GSF_RENDER_COPY: return "copy";
    case GSF_RENDER_DIRECT: return "direct";
    case GSF_RENDER_BATCH: return "batch";
    default: return "unavailable";
    }
}

static inline double gsf_renderer_score(const struct gsf_renderer_choice *s,uint32_t mode)
{
    double v[GSF_RENDER_SAMPLES];
    memcpy(v,s->costs[mode],sizeof v);
    for(uint32_t i=1;i<GSF_RENDER_SAMPLES;i++) {
        double x=v[i];uint32_t j=i;
        while(j>0 && v[j-1]>x) {v[j]=v[j-1];j--;}
        v[j]=x;
    }
    /* Mostly median; penalize a recurring slow tail, tolerate one scheduling outlier. */
    return (.375*(v[3]+v[4])+.25*v[6]);
}

static inline void gsf_renderer_reset(struct gsf_renderer_choice *s,uint32_t available)
{
    uint32_t epoch=s->epoch+1u;enum gsf_renderer incumbent=s->winner;
    memset(s,0,sizeof *s);s->epoch=epoch;s->available=available;
    s->winner=(incumbent>=0 && (available & (1u<<(uint32_t)incumbent))) ? incumbent : GSF_RENDER_COPY;
}

static inline enum gsf_renderer gsf_renderer_pick(struct gsf_renderer_choice *s,uint32_t available)
{
    if(s->available!=available) gsf_renderer_reset(s,available);
    if(s->settled) return s->winner;
    if(s->issued>=128) return GSF_RENDER_COPY; /* bounded exploration without reliable timestamps */
    uint32_t n=0;
    for(uint32_t k=0;k<GSF_RENDER_COUNT;k++) if(available & (1u<<k)) n++;
    if(n==0) return GSF_RENDER_COPY;
    uint32_t sequence=s->issued++;
    /* Rotate each balanced round: a fixed order aliases three work slots and
     * two history parities, incorrectly attributing their cost to the renderer. */
    uint32_t turn=(sequence%n+(sequence/n)%n)%n;
    for(uint32_t k=0;k<GSF_RENDER_COUNT;k++)
        if((available & (1u<<k)) && turn--==0) return (enum gsf_renderer)k;
    return GSF_RENDER_COPY;
}

static inline void gsf_renderer_observe(struct gsf_renderer_choice *s,uint32_t epoch,
                                        enum gsf_renderer mode,double cost_ms)
{
    if(epoch!=s->epoch || mode<0 || mode>=GSF_RENDER_COUNT ||
       !(s->available & (1u<<(uint32_t)mode)) || !(cost_ms>0 && cost_ms<1000)) return;
    uint32_t k=(uint32_t)mode;
    s->costs[k][s->position[k]++ % GSF_RENDER_SAMPLES]=cost_ms;
    if(s->samples[k]<GSF_RENDER_SAMPLES) s->samples[k]++;
    if(s->settled) {
        if(mode!=s->winner) return;
        s->locked_samples++;
        s->shock=cost_ms>s->baseline*1.5+.05 ? s->shock+1u : 0u;
        if(s->locked_samples>=128 && s->shock>=GSF_RENDER_SAMPLES)
            gsf_renderer_reset(s,s->available); /* old in-flight epochs are ignored */
        return;
    }
    for(uint32_t j=0;j<GSF_RENDER_COUNT;j++)
        if((s->available & (1u<<j)) && s->samples[j]<GSF_RENDER_SAMPLES) return;
    enum gsf_renderer best=s->winner;
    double previous=gsf_renderer_score(s,(uint32_t)best),score=previous;
    for(uint32_t j=0;j<GSF_RENDER_COUNT;j++) if(s->available & (1u<<j)) {
        double candidate=gsf_renderer_score(s,j);
        if(candidate<score) {best=(enum gsf_renderer)j;score=candidate;}
    }
    /* A small/noisy difference is not a reason to switch the incumbent. */
    if(score<previous*.9-.02) s->winner=best;
    s->baseline=gsf_renderer_score(s,(uint32_t)s->winner);s->settled=true;
}

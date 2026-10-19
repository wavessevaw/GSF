/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only 0.1.9 shadow admission. This policy never controls real dispatch.
 * Only already-observed calibrated GPU-start samples may enter the history.
 */
#ifndef GSF_SHADOW_ADMISSION_H
#define GSF_SHADOW_ADMISSION_H
#include <stdint.h>
#include <string.h>
enum { GSF_SHADOW_CAP=8, GSF_SHADOW_STREAK=10 };
typedef struct {uint64_t latency_ns,start_ns,observed_ns,te_sequence;} gsf_shadow_sample;
typedef struct {
    gsf_shadow_sample ring[GSF_SHADOW_CAP];
    unsigned next,count,skip_streak;
    uint64_t last_would_probe_ns;
} gsf_shadow_history;
typedef struct {
    const char *prediction,*reason;
    uint64_t last_latency_ns,last_gpu_start_ns,age_ns,median3_ns,median5_ns,last_sequence;
    unsigned history_samples,fresh_samples,skip_streak,would_probe;
    int last_valid,median3_valid,median5_valid;
} gsf_shadow_decision;
static inline void gsf_shadow_record(gsf_shadow_history *h,uint64_t sequence,
    uint64_t start_ns,uint64_t latency_ns,uint64_t observed_ns) {
    if(!sequence||!observed_ns)return;
    if(h->count && h->ring[(h->next+GSF_SHADOW_CAP-1u)%GSF_SHADOW_CAP].te_sequence==sequence)
        return;
    h->ring[h->next]=(gsf_shadow_sample){latency_ns,start_ns,observed_ns,sequence};
    h->next=(h->next+1u)%GSF_SHADOW_CAP;
    if(h->count<GSF_SHADOW_CAP)++h->count;
}
static inline uint64_t gsf_shadow_median(uint64_t *a,unsigned n) {
    for(unsigned i=1;i<n;++i){uint64_t v=a[i];unsigned j=i;
        while(j && a[j-1]>v){a[j]=a[j-1];--j;}a[j]=v;}
    return a[n/2];
}
/* The classifier never looks ahead. Stale history => unknown.
 * A probe is a hypothetical shadow label, not an actual submission. */
static inline gsf_shadow_decision gsf_shadow_predict(gsf_shadow_history *h,
    uint64_t now,int has_candidate) {
    gsf_shadow_decision d={.prediction="unknown",.reason="no_history"};
    d.history_samples=h->count;
    uint64_t values[5]={0};unsigned n=0;
    for(unsigned i=0;i<h->count;++i){
        const gsf_shadow_sample *s=&h->ring[(h->next+GSF_SHADOW_CAP-1u-i)%GSF_SHADOW_CAP];
        if(s->observed_ns>now)continue;
        if(!d.last_valid){
            d.last_valid=1;d.last_latency_ns=s->latency_ns;
            d.last_gpu_start_ns=s->start_ns;d.age_ns=now-s->observed_ns;
            d.last_sequence=s->te_sequence;
        }
        if(n<5)values[n++]=s->latency_ns;
    }
    d.fresh_samples=n;
    if(n>=3){uint64_t a[3];memcpy(a,values,sizeof(a));d.median3_ns=gsf_shadow_median(a,3);d.median3_valid=1;}
    if(n>=5){uint64_t a[5];memcpy(a,values,sizeof(a));d.median5_ns=gsf_shadow_median(a,5);d.median5_valid=1;}
    if(!has_candidate){d.reason="not_candidate";return d;}
    if(d.last_valid && d.age_ns<=500000000ull && d.last_latency_ns<5000000ull) {
        d.prediction="admit";d.reason="last_gpu_start_fast";h->skip_streak=0;
    } else {
        if(!d.last_valid){d.prediction="unknown";d.reason="no_history";}
        else if(d.age_ns>500000000ull){d.prediction="unknown";d.reason="stale_measurement";}
        else {d.prediction="skip";d.reason="last_gpu_start_slow";}
        /* Read-only recovery label: no actual TE-submit, even on probe. */
        if(h->skip_streak<UINT32_MAX)++h->skip_streak;
        if(h->skip_streak>=GSF_SHADOW_STREAK &&
           (!h->last_would_probe_ns || now>=h->last_would_probe_ns+500000000ull)) {
            d.would_probe=1;d.prediction="admit_probe";
            h->last_would_probe_ns=now;h->skip_streak=0;
        }
    }
    d.skip_streak=h->skip_streak;
    return d;
}
#endif

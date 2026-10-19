/* SPDX-License-Identifier: GPL-3.0-or-later
 * Deterministic 45 Hz source, 90 Hz output, start latency 0.1->35/55->0.15ms.
 * Prediction is read-only and does not change the independent samples.
 */
#include "../src/shadow_admission.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef struct {uint64_t ready,start,delay;unsigned seq;} item;
int main(void) {
    gsf_shadow_history h={0};
    item pending[8]={0};unsigned count=0;
    unsigned phase[3][4]={{0}},probes=0;
    for(unsigned i=0;i<2700;++i) {
        uint64_t now=2000000000ull+(uint64_t)i*22222222ull;
        for(unsigned k=0;k<count;) {
            if(pending[k].ready>now){++k;continue;}
            gsf_shadow_record(&h,pending[k].seq,pending[k].start,
                              pending[k].delay,pending[k].ready);
            pending[k]=pending[--count];
        }
        gsf_shadow_decision d=gsf_shadow_predict(&h,now,1);
        unsigned z=i<450?0:i<1800?1:2;phase[z][0]++;
        if(!strcmp(d.prediction,"admit"))phase[z][1]++;
        if(!strcmp(d.prediction,"skip"))phase[z][2]++;
        if(d.would_probe){phase[z][3]++;++probes;}
        uint64_t delay=z==0?100000ull:z==1?
            35000000ull+(uint64_t)(i%3)*10000000ull:150000ull;
        if(count<8) pending[count++]=(item){now+delay+6000000ull,now+delay,delay,i+1};
    }
    assert(phase[0][1]>100 && phase[1][2]>100 && phase[2][1]>100);
    assert(probes>0 && probes<100);
    printf("shadow-stress PASS warm_admit=%u congestion_skip=%u recovery_admit=%u hypothetical_probes=%u\n",
        phase[0][1],phase[1][2],phase[2][1],probes);
    return 0;
}

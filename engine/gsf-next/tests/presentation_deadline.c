/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "presentation_deadline.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint64_t tick=gsf_refresh_period(90),epoch=1000000000ull;
    const uint64_t synth=epoch+tick,real=epoch+2*tick;
    assert(gsf_synthetic_deadline_check(epoch,synth,real,synth,0,0,tick,90,false)==GSF_SYNTHETIC_ON_TIME);
    assert(gsf_synthetic_deadline_check(epoch,synth,real,synth+tick/2,0,0,tick,90,false)==GSF_SYNTHETIC_ON_TIME);
    assert(gsf_synthetic_deadline_check(epoch,synth,real,synth+tick/2+1,0,0,tick,90,false)==GSF_SYNTHETIC_EXPIRED);
    assert(gsf_synthetic_deadline_check(epoch,synth,real,synth,0,0,tick,90,true)==GSF_SYNTHETIC_SUPERSEDED);
    assert(gsf_synthetic_deadline_check(epoch,real,real,synth,0,0,tick,90,false)==GSF_SYNTHETIC_EXPIRED);
    assert(gsf_synthetic_deadline_check(epoch,synth,real,synth,synth,real-tick/2,tick,90,false)==GSF_SYNTHETIC_EXPIRED);
    for(uint32_t hz=60;hz<=120;hz+=30) {
        uint64_t t=gsf_refresh_period(hz),e=1000000000ull;
        for(uint32_t i=0;i<1000;i++) {
            uint64_t a=e+(uint64_t)i*3*t, b=a+t, r=a+2*t;
            assert(gsf_synthetic_deadline_check(e,b,r,b,0,0,t,hz,false)==GSF_SYNTHETIC_ON_TIME);
            assert(gsf_synthetic_deadline_check(e,b,r,b+t,0,0,t,hz,false)==GSF_SYNTHETIC_EXPIRED);
        }
    }
    puts("real-priority presentation deadlines: PASS");
}

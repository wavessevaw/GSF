/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "queue_depth.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(gsf_queue_desired(0,90,2)==2);
    assert(gsf_queue_desired(0,90,3)==2);
    assert(gsf_queue_desired(0,90,4)==2);
    assert(gsf_queue_desired(0,120,4)==2);
    assert(gsf_queue_desired(0,144,4)==2);
    assert(gsf_queue_desired(0,240,4)==2);
    assert(gsf_queue_desired(0,40,2)==2);
    assert(gsf_queue_playout_ticks(1,3)==4);
    assert(gsf_queue_playout_ticks(1,5)==6);
    assert(gsf_queue_playout_ticks(2,7)==8);
    assert(gsf_queue_playout_ticks(12,8)==9);
    assert(gsf_queue_playout_ticks(6,2)==6);
    assert(gsf_queue_effective(0,240,4,6)==2);
    assert(gsf_queue_effective(8,90,2,3)==3);
    assert(gsf_queue_effective(1,240,4,8)==1);
    assert(gsf_queue_effective(0,90,3,0)==1);
    for(uint32_t hz=40;hz<=240;hz++)
        for(uint32_t m=2;m<=4;m++)
            for(uint32_t c=1;c<=8;c++) {
                uint32_t q=gsf_queue_effective(0,hz,m,c);
                assert(q>=1 && q<=c && q<=8);
                if(hz>40) assert(gsf_queue_desired(0,hz,m)>=gsf_queue_desired(0,hz-1,m));
                if(m>2) assert(gsf_queue_desired(0,hz,m)>=gsf_queue_desired(0,hz,m-1));
            }
    puts("conservative automatic output queue with manual override and physical clamp: PASS");
}

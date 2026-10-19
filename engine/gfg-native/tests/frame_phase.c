/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../src/gpu_bridge.c"
#include <assert.h>
#include <math.h>
static void rates(void) {
    const unsigned hz[]={30,45,60,75,90};
    for(unsigned output=60;output<=90;output+=30)for(unsigned m=2;m<=3;++m)
    for(unsigned f=0;f<5;++f)for(unsigned jitter=0;jitter<=3000000;jitter=jitter?jitter==200000?1000000:jitter==1000000?3000000:3000001:200000) {
        gfg_gpu_fg fg={.same_device=1,.multiplier=m,.clock={.period_ns=1000000000ull/output}};
        uint64_t source=10000000000ull;unsigned checked=0;
        for(unsigned i=0;i<1000;++i) {
            source+=1000000000ull/hz[f];
            uint64_t at=source+(i%2?jitter:0);
            gfg_frame_clock_pair(&fg.clock,at,m);
            te_submit s={0};unsigned mask=prepare_generation(&fg,1,10,at,&s);
            unsigned out=0;
            for(unsigned k=0;k<m-1;++k)if(mask&(1u<<k)) {
                const double expected=(double)(fg.clock.target_ns[k]-fg.clock.previous_real_ns)/
                    (double)(fg.clock.target_ns[2]-fg.clock.previous_real_ns);
                assert(fabs((double)s.timestamps[out]-expected)<0.000001);
                assert(s.timestamps[out]>0 && s.timestamps[out]<1);
                if(out)assert(s.timestamps[out]>s.timestamps[out-1]);
                ++out;++checked;
            }
            assert(s.count==out && s.wait_value==10 && s.signal_first==(out?13-out:12));
            if(output==90 && hz[f]==45 && m==3 && !jitter && i>2) {
                assert(mask==2 && s.count==1 && s.timestamps[0]==0.5f);
            }
            if(output==90 && hz[f]==30 && m==3 && !jitter && i>2) {
                assert(mask==3 && s.count==2);
                assert(fabs(s.timestamps[0]-1.f/3)<0.000001 && fabs(s.timestamps[1]-2.f/3)<0.000001);
            }
        }
        if(hz[f]>=output && !jitter)assert(checked==0);
    }
}
int main(int argc,char **argv) {
    assert(argc==2);rates();
    FILE *f=fopen(argv[1],"r");assert(f);char line[1024];assert(fgets(line,sizeof(line),f));
    unsigned rows=0,half=0;
    while(fgets(line,sizeof(line),f)) {
        char *fields[13],*save=NULL;unsigned n=0;
        for(char *p=strtok_r(line,",",&save);p && n<13;p=strtok_r(NULL,",",&save))fields[n++]=p;
        assert(n==13);
        gfg_frame_clock c={.previous_real_ns=strtoull(fields[6],NULL,10),.mask=(unsigned)strtoul(fields[10],NULL,10)};
        const unsigned slot=(unsigned)strtoul(fields[8],NULL,10);
        c.target_ns[slot]=strtoull(fields[9],NULL,10);c.target_ns[2]=strtoull(fields[7],NULL,10);
        float tau=0;assert(gfg_frame_clock_phase(&c,slot,&tau));
        assert(fabs(tau-strtod(fields[12],NULL))<0.000001);++rows;
        if(c.mask==2 && fabs(tau-.5)<0.000001)++half;
    }
    fclose(f);assert(rows>8000 && half==4433);
    puts("frame-phase PASS: actual prepare_generation x2/x3, masks, signals, jitter/rate grid; all retained phase rows; 4433 mask2 midpoints");
    return 0;
}

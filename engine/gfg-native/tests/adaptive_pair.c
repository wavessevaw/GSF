#include "../src/adaptive_pair.h"
#include <stdio.h>
#include <assert.h>
#define MS 1000000ull
int main(void){
 gsf_adaptive_pair s;gsf_ap_init(&s);
 const uint64_t o=1000*MS,p=11*MS;
 gsf_pair_plan z=gsf_ap_begin(&s,1001*MS,1,o,p,1022*MS,1033*MS,49*MS,1,1,0,0);
 assert(!z.admit); /* original structural miss cannot be fixed by shader speed */
 z=gsf_ap_begin(&s,1600*MS,2,o,p,1622*MS,1633*MS,49*MS,1,1,0,0);
 assert(z.admit&&z.probe); /* never permanently stranded by stale estimate */
 uint64_t a,b;
 assert(gsf_ap_ready(&s,1618*MS,&a,&b)&&a==z.g1&&b==z.real);
 assert(!gsf_ap_ready(&s,1624*MS,&a,&b)); /* no ready-time retarget */
 assert(!gsf_ap_ready(&s,1660*MS,&a,&b)); /* no unlimited real-frame hold */
 for(int i=0;i<3;++i)gsf_ap_outcome(&s,1633*MS+i*MS,1,0);
 assert(s.state==GSF_DEGRADED);
 assert(!gsf_ap_begin(&s,1650*MS,3,o,p,1666*MS,1677*MS,4*MS,1,1,0,0).admit);
 z=gsf_ap_begin(&s,2650*MS,4,o,p,2666*MS,2677*MS,4*MS,1,1,0,0);
 assert(z.admit&&z.probe);
 assert(gsf_ap_ready(&s,2658*MS,&a,&b));
 gsf_ap_outcome(&s,2659*MS,1,1);
 z=gsf_ap_begin(&s,2675*MS,5,o,p,2700*MS,2711*MS,4*MS,1,1,0,0);
 assert(z.admit);
 assert(gsf_ap_ready(&s,2683*MS,&a,&b));
 gsf_ap_outcome(&s,2684*MS,1,1);assert(s.state==GSF_GENERATING);
 assert(!gsf_ap_begin(&s,2700*MS,6,o,p,2722*MS,2733*MS,4*MS,1,1,1,0).admit);
 assert(!gsf_ap_begin(&s,2725*MS,7,o,p,2744*MS,2755*MS,4*MS,1,0,0,1).admit);
 assert(s.state==GSF_SYNCING);
 unsigned successes=0;
 for(unsigned i=0;i<2200;i++) {
  uint64_t n=(3000ull+(uint64_t)i*25)*MS;
  uint64_t b0=n+33*MS;
  int heavy=(i%280)>=85&&(i%280)<145;
  z=gsf_ap_begin(&s,n,i+8,o,p,n+22*MS,b0,heavy?45*MS:5*MS,1,1,0,0);
  if(z.admit) {
   int good=gsf_ap_ready(&s,n+(heavy?50:8)*MS,&a,&b);
   assert(!good||(a<b&&a==z.g1&&b==b0));
   gsf_ap_outcome(&s,n+20*MS,1,good);successes+=good;
  } else gsf_ap_outcome(&s,n+20*MS,0,0);
 }
 assert(s.cooldown_exits>1 && s.generation_restarts>1);
 gsf_ap_init(&s);
 s.eligible=1;gsf_ap_outcome(&s,99999999999ull,1,1);
 assert(s.state!=GSF_GENERATING && !s.stable_entries); /* accepted without readiness is not recovery */
 assert(successes>400);
 printf("adaptive-pair PASS 2200 variable-source cycles accepted=%u state=%s immutable_B=baseline\n",successes,gsf_ap_name(s.state));
}

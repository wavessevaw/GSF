/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_config.h"
#include <stdio.h>
#include <stdlib.h>

struct afmf_config gsf_live_config(void)
{
    struct afmf_config cfg=*afmf_config_get();
    const char *path=getenv("GSF_NEXT_CONTROL");
    if(path==NULL) return cfg;
    FILE *in=fopen(path,"r");
    if(in==NULL) return cfg;
    char text[512];
    size_t size=fread(text,1,sizeof text-1,in);
    bool complete=feof(in)!=0;
    (void)fclose(in);text[size]='\0';
    unsigned long long revision=0;
    unsigned enabled=0,multiplier=0,hz=0,quality=0,queue=0,images=0,mode=0,diagnostics=0;
    int used=0;
    int fields=sscanf(text,"GSF_NEXT_CONTROL_V1\n%llu %u %u %u %u %u %u %u %u %n",
        &revision,&enabled,&multiplier,&hz,&quality,&queue,&images,&mode,&diagnostics,&used);
    if(!complete || fields!=9 || used<=0 || (size_t)used!=size || enabled>1 ||
       (multiplier!=0 && multiplier!=2 && multiplier!=3 && multiplier!=4) || hz<30 || hz>360 ||
       (quality!=1 && quality!=2) || queue<1 || queue>3 || (images!=3 && images!=5) || mode>1 || diagnostics>1)
        return cfg;
    cfg.revision=(uint64_t)revision;cfg.enabled=enabled!=0 && multiplier!=0;
    cfg.multiplier=multiplier;cfg.refresh_hz=hz;cfg.performance=(enum afmf_performance_mode)quality;
    cfg.max_in_flight=queue;cfg.extra_images=images;cfg.present_mode=(enum afmf_present_mode)mode;
    cfg.diagnostics=diagnostics!=0;
    return cfg;
}

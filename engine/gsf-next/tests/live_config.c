/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static const struct afmf_config base={.multiplier=2,.enabled=true,.refresh_hz=90,.max_in_flight=2};
const struct afmf_config *afmf_config_get(void) { return &base; }
int main(void)
{
    char path[]="/tmp/gsf-control-XXXXXX";int fd=mkstemp(path);assert(fd>=0);(void)close(fd);
    assert(setenv("GSF_NEXT_CONTROL",path,1)==0);
    const char *rows[]={"", "GSF_NEXT_CONTROL_V1\n9 1 4 90 2 2 3 0 1\n", "GSF_NEXT_CONTROL_V1\n10 0 0 60 1 1 5 1 0\n",
        "GSF_NEXT_CONTROL_V1\n11 1 1 90 2 2 3 0 1\n", "GSF_NEXT_CONTROL_V1\n12 1 4 90 2 99 3 0 1\n", "GSF_NEXT_CONTROL_V1\n13 1 4 90 2 2 3 0 1\nextra", "GSF_NEXT_CONTROL_V1\n14 1 4"};
    for(unsigned k=0;k<sizeof rows/sizeof rows[0];k++) {
        FILE *out=fopen(path,"w");assert(out!=NULL);assert(fputs(rows[k],out)>=0);assert(fclose(out)==0);
        struct afmf_config cfg=gsf_live_config();
        if(k==1) assert(cfg.revision==9 && cfg.enabled && cfg.multiplier==4 && cfg.diagnostics);
        else if(k==2) assert(cfg.revision==10 && !cfg.enabled && cfg.refresh_hz==60 && cfg.extra_images==5);
        else assert(cfg.revision==0 && cfg.multiplier==2 && cfg.enabled);
    }
    assert(unlink(path)==0);assert(gsf_live_config().multiplier==2);
    puts("bounded atomic live control: PASS");
}

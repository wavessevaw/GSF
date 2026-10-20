/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain_internal.h"

/* Only worker calls this. Finish existing output before replacing a GPU context;
 * the application's present hook never waits directly for reconfiguration. */
void afmf_sc_live_update(struct afmf_device *dev,struct afmf_swapchain *sc,uint64_t now)
{
    if(now-sc->config_poll_ns<100000000ull) return;
    sc->config_poll_ns=now;
    struct afmf_config next=gsf_live_config();
    if(next.revision<=sc->config.revision) return;
    bool recreate=next.extra_images!=sc->wsi_extra_images || next.present_mode!=sc->wsi_present_mode;
    if(!recreate && next.performance!=sc->config.performance) {
        pthread_mutex_lock(&sc->job_lock);
        while(sc->job_count>0) pthread_cond_wait(&sc->drain_cond,&sc->job_lock);
        pthread_mutex_unlock(&sc->job_lock);
        for(uint32_t k=0;k<sc->image_count;k++)
            if(sc->slots[k].pending && dev->fns.wait_for_fences(dev->handle,1,&sc->slots[k].fence,VK_TRUE,UINT64_MAX)!=VK_SUCCESS)
                return;
        struct afmf_framegen *replacement=afmf_framegen_create(dev,sc->format,sc->extent,sc->image_count,
            sc->images,sc->image_count,false,sc->sampled_usage,&next);
        if(replacement==NULL) return;
        afmf_framegen_destroy(dev,sc->fg);sc->fg=replacement;sc->have_history=false;
    }
    afmf_framegen_set_diagnostics(sc->fg,next.diagnostics);
    pthread_mutex_lock(&sc->job_lock);
    sc->recreate_requested=recreate;
    sc->config=next;
    if(!next.enabled) sc->have_history=false;
    pthread_cond_broadcast(&sc->drain_cond);
    pthread_mutex_unlock(&sc->job_lock);
}

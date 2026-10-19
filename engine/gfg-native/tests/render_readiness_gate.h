/* SPDX-License-Identifier: GPL-3.0-or-later
 * TEST APPLICATION ONLY. Delay its rendering dependency, not the GSF layer.
 * Host-signaled VkEvents avoid an unsubmitted timeline signal dependency in
 * vkQueuePresentKHR. Binary render-finished signals are submitted normally.
 */
#ifndef GSF_TEST_RENDER_READINESS_GATE_H
#define GSF_TEST_RENDER_READINESS_GATE_H
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkDevice device;
    VkEvent events[2];
    uint64_t due[2], armed, released;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    int stop, failed;
} render_readiness_gate;

static uint64_t rrg_now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
static void *rrg_thread(void *p) {
    render_readiness_gate *g=p;
    pthread_mutex_lock(&g->lock);
    while(!g->stop) {
        unsigned k=2;
        for(unsigned i=0;i<2;++i)if(g->due[i] && (k==2 || g->due[i]<g->due[k]))k=i;
        if(k==2){pthread_cond_wait(&g->changed,&g->lock);continue;}
        if(rrg_now()<g->due[k]) {
            struct timespec t={(time_t)(g->due[k]/1000000000ull),(long)(g->due[k]%1000000000ull)};
            pthread_cond_timedwait(&g->changed,&g->lock,&t);continue;
        }
        /* Serialize host event set/reset. The application also waits its
         * render fence before rearming this event and command-buffer slot. */
        if(vkSetEvent(g->device,g->events[k])!=VK_SUCCESS)g->failed=1;
        g->due[k]=0;++g->released;
    }
    pthread_mutex_unlock(&g->lock);return NULL;
}
static render_readiness_gate *rrg_start(VkDevice device) {
    render_readiness_gate *g=calloc(1,sizeof(*g));
    if(!g)return NULL;
    g->device=device;
    if(pthread_mutex_init(&g->lock,NULL)){free(g);return NULL;}
    pthread_condattr_t a;
    if(pthread_condattr_init(&a)){pthread_mutex_destroy(&g->lock);free(g);return NULL;}
    int error=pthread_condattr_setclock(&a,CLOCK_MONOTONIC);
    if(!error)error=pthread_cond_init(&g->changed,&a);
    pthread_condattr_destroy(&a);
    if(error){pthread_mutex_destroy(&g->lock);free(g);return NULL;}
    VkEventCreateInfo ci={.sType=VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    for(unsigned i=0;i<2;++i)if(vkCreateEvent(device,&ci,NULL,&g->events[i])!=VK_SUCCESS)goto fail;
    if(pthread_create(&g->thread,NULL,rrg_thread,g))goto fail;
    return g;
fail:
    for(unsigned i=0;i<2;++i)if(g->events[i])vkDestroyEvent(device,g->events[i],NULL);
    pthread_cond_destroy(&g->changed);pthread_mutex_destroy(&g->lock);free(g);return NULL;
}
static int rrg_arm(render_readiness_gate *g,unsigned slot,uint64_t delay_ns) {
    pthread_mutex_lock(&g->lock);
    int ok=slot<2 && !g->due[slot] && !g->failed;
    if(ok)ok=vkResetEvent(g->device,g->events[slot])==VK_SUCCESS;
    if(ok){g->due[slot]=rrg_now()+delay_ns;++g->armed;pthread_cond_signal(&g->changed);}
    pthread_mutex_unlock(&g->lock);return ok;
}
/* Call after application GPU work has drained, while its VkDevice is alive. */
static int rrg_stop(render_readiness_gate *g,FILE *trace) {
    if(!g)return 1;
    pthread_mutex_lock(&g->lock);g->stop=1;pthread_cond_signal(&g->changed);
    pthread_mutex_unlock(&g->lock);pthread_join(g->thread,NULL);
    int ok=!g->failed && g->armed==g->released;
    if(trace)fprintf(trace,"{\"event\":\"gate_summary\",\"armed\":%llu,\"released\":%llu,\"ok\":%d}\n",
        (unsigned long long)g->armed,(unsigned long long)g->released,ok);
    printf("GSF_RENDER_READY_GATE armed=%llu released=%llu status=%s\n",
        (unsigned long long)g->armed,(unsigned long long)g->released,ok?"PASS":"FAIL");
    for(unsigned i=0;i<2;++i)vkDestroyEvent(g->device,g->events[i],NULL);
    pthread_cond_destroy(&g->changed);pthread_mutex_destroy(&g->lock);free(g);return ok;
}
#endif

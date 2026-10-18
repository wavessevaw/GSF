/* SPDX-License-Identifier: GPL-3.0-or-later
 * Diagnostic-only timeline observer. Does not submit GPU work, own semaphore,
 * take queue/WSI locks or modify output admission. All times are CLOCK_MONOTONIC.
 */
#ifndef GSF_TIMELINE_WATCH_H
#define GSF_TIMELINE_WATCH_H
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TW_CAP 128u
#define TW_LIMIT_NS 200000000ull
#define TW_POLL_NS 2000000ull

typedef struct {
    uint64_t base, sequence, threshold, signal, registered_ns, submit_ns;
    uint64_t deadline_ns, last_below_ns;
    unsigned output_index, deadline_kind; /* 0 scheduled, 1 visibility-probe */
    unsigned state; /* 0 free, 1 registered, 2 submitted, 3 completed */
} tw_record;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    VkDevice device;
    VkSemaphore semaphore;
    PFN_vkWaitSemaphores wait;
    PFN_vkGetSemaphoreCounterValue counter;
    FILE *log; /* owner joins observer before fclose */
    tw_record entries[TW_CAP];
    int stop;
    uint64_t overflow, wakeups, timed_out, signaled, expired, failed, teardown;
} timeline_watch;

static uint64_t tw_now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}

static void tw_finish_locked(timeline_watch *w, tw_record *r,const char *outcome,
                             uint64_t observed,uint64_t value) {
    if(r->state==3)return;
    int64_t over=r->deadline_ns ?
        (int64_t)observed-(int64_t)r->deadline_ns : 0;
    if(!strcmp(outcome,"threshold_reached"))++w->signaled;
    else if(!strcmp(outcome,"expired"))++w->expired;
    else if(!strcmp(outcome,"submit_failed"))++w->failed;
    else if(!strcmp(outcome,"teardown"))++w->teardown;
    if(w->log)fprintf(w->log,
        "{\"schema\":1,\"event\":\"timeline_watch\",\"te_sequence\":%llu,"
        "\"base_value\":%llu,\"output_index\":%u,\"wait_threshold\":%llu,"
        "\"actual_signal_value\":%llu,\"signal_source\":\"gpu_queue\","
        "\"submit_status\":\"%s\",\"deadline_kind\":\"%s\","
        "\"watch_end_reason\":\"%s\",\"registered_ns\":%llu,\"submit_ns\":%llu,"
        "\"deadline_ns\":%llu,\"last_below_ns\":%llu,\"timeline_observed_ns\":%llu,"
        "\"timeline_value_seen\":%llu,\"completion_overrun_ns\":%lld,"
        "\"observation_max_ns\":%llu,\"watch_poll_interval_ns\":%llu}\n",
        (unsigned long long)r->sequence,(unsigned long long)r->base,r->output_index,
        (unsigned long long)r->threshold,(unsigned long long)r->signal,
        !strcmp(outcome,"submit_failed")?"failed":r->submit_ns?"submitted":"pending",
        r->deadline_kind?"visibility_probe":"scheduled",outcome,
        (unsigned long long)r->registered_ns,(unsigned long long)r->submit_ns,
        (unsigned long long)r->deadline_ns,(unsigned long long)r->last_below_ns,
        (unsigned long long)observed,(unsigned long long)value,(long long)over,
        (unsigned long long)TW_LIMIT_NS,(unsigned long long)TW_POLL_NS);
    r->state=3;
}

static void *tw_thread_main(void *ptr) {
    timeline_watch *w=ptr;
    pthread_mutex_lock(&w->lock);
    for(;;) {
        if(w->stop)break;
        tw_record *chosen=NULL;
        uint64_t n=tw_now();
        for(unsigned i=0;i<TW_CAP;++i) {
            tw_record *r=&w->entries[i];
            if(r->state!=2)continue;
            if(n-r->submit_ns>=TW_LIMIT_NS) {
                tw_finish_locked(w,r,"expired",n,0);continue;
            }
            if(!chosen || r->signal<chosen->signal)chosen=r;
        }
        if(!chosen) {
            pthread_cond_wait(&w->changed,&w->lock);
            continue;
        }
        uint64_t target=chosen->signal;
        uint64_t base=chosen->base;
        unsigned oi=chosen->output_index;
        VkSemaphoreWaitInfo info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount=1,.pSemaphores=&w->semaphore,.pValues=&target};
        pthread_mutex_unlock(&w->lock);
        VkResult result=w->wait(w->device,&info,TW_POLL_NS);
        uint64_t observed=tw_now(),current=0;
        if(result==VK_SUCCESS && w->counter)
            if(w->counter(w->device,w->semaphore,&current)!=VK_SUCCESS)current=0;
        pthread_mutex_lock(&w->lock);
        ++w->wakeups;
        if(result==VK_TIMEOUT)++w->timed_out;
        tw_record *r=NULL;
        for(unsigned i=0;i<TW_CAP;++i)if(w->entries[i].state==2 &&
            w->entries[i].base==base && w->entries[i].output_index==oi){r=&w->entries[i];break;}
        if(r) {
            if(result==VK_SUCCESS)tw_finish_locked(w,r,"threshold_reached",observed,current);
            else if(result==VK_TIMEOUT)r->last_below_ns=observed;
            else tw_finish_locked(w,r,"device_error",observed,0);
        }
    }
    for(unsigned i=0;i<TW_CAP;++i)if(w->entries[i].state==1 || w->entries[i].state==2)
        tw_finish_locked(w,&w->entries[i],"teardown",tw_now(),0);
    if(w->log)fprintf(w->log,"{\"schema\":1,\"event\":\"timeline_watch_summary\","
        "\"threshold_reached\":%llu,\"expired\":%llu,\"submit_failed\":%llu,"
        "\"teardown\":%llu,\"watch_queue_overflow\":%llu,\"watch_wakeups\":%llu,"
        "\"watch_timeouts\":%llu}\n",
        (unsigned long long)w->signaled,(unsigned long long)w->expired,
        (unsigned long long)w->failed,(unsigned long long)w->teardown,
        (unsigned long long)w->overflow,(unsigned long long)w->wakeups,
        (unsigned long long)w->timed_out);
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

static timeline_watch *tw_start(VkDevice device,VkSemaphore semaphore,
    PFN_vkWaitSemaphores wait,PFN_vkGetSemaphoreCounterValue counter,FILE *log) {
    if(!device || !semaphore || !wait || !log)return NULL;
    timeline_watch *w=calloc(1,sizeof(*w));
    if(!w)return NULL;
    w->device=device;w->semaphore=semaphore;w->wait=wait;w->counter=counter;w->log=log;
    if(pthread_mutex_init(&w->lock,NULL)) {free(w);return NULL;}
    if(pthread_cond_init(&w->changed,NULL)) {pthread_mutex_destroy(&w->lock);free(w);return NULL;}
    if(pthread_create(&w->thread,NULL,tw_thread_main,w)) {
        pthread_cond_destroy(&w->changed);pthread_mutex_destroy(&w->lock);free(w);return NULL;
    }
    return w;
}

/* Register before TE submission. A failed submission must be closed explicitly. */
static void tw_register(timeline_watch *w,uint64_t base,uint64_t sequence,
    unsigned output_index,uint64_t threshold,uint64_t signal,uint64_t deadline,int probe) {
    if(!w)return;
    pthread_mutex_lock(&w->lock);
    tw_record *r=NULL;
    for(unsigned i=0;i<TW_CAP;++i)if(w->entries[i].state==0 || w->entries[i].state==3) {
        r=&w->entries[i];break;
    }
    if(r) {
        *r=(tw_record){.base=base,.sequence=sequence,.output_index=output_index,
            .threshold=threshold,.signal=signal,.deadline_ns=deadline,
            .deadline_kind=(unsigned)(probe!=0),.registered_ns=tw_now(),.state=1};
    } else {
        ++w->overflow;
        if(w->log)fprintf(w->log,"{\"schema\":1,\"event\":\"timeline_watch_overflow\",\"watch_queue_overflow\":%llu}\n",
            (unsigned long long)w->overflow);
    }
    pthread_mutex_unlock(&w->lock);
}

static void tw_submitted(timeline_watch *w,uint64_t base,int succeeded) {
    if(!w)return;
    pthread_mutex_lock(&w->lock);
    for(unsigned i=0;i<TW_CAP;++i) {
        tw_record *r=&w->entries[i];
        if(r->state!=1 || r->base!=base)continue;
        r->submit_ns=tw_now();
        if(succeeded)r->state=2;
        else tw_finish_locked(w,r,"submit_failed",r->submit_ns,0);
    }
    pthread_cond_signal(&w->changed);
    pthread_mutex_unlock(&w->lock);
}

static void tw_deadline(timeline_watch *w,uint64_t base,unsigned output_index,uint64_t actual_deadline) {
    if(!w)return;
    pthread_mutex_lock(&w->lock);
    for(unsigned i=0;i<TW_CAP;++i) {
        tw_record *r=&w->entries[i];
        if(r->base==base && r->output_index==output_index && r->state==2)
            r->deadline_ns=actual_deadline;
    }
    pthread_mutex_unlock(&w->lock);
}

static void tw_stop(timeline_watch *w) {
    if(!w)return;
    pthread_mutex_lock(&w->lock);w->stop=1;pthread_cond_broadcast(&w->changed);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL);
    pthread_cond_destroy(&w->changed);pthread_mutex_destroy(&w->lock);
    free(w);
}
#endif

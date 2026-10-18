/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/timeline_watch.h"
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

static atomic_uint_fast64_t fake_timeline;
static VkResult fake_wait(VkDevice dev,const VkSemaphoreWaitInfo *info,uint64_t timeout) {
    (void)dev;
    uint64_t started=tw_now();
    while (tw_now()-started < timeout) {
        if(atomic_load(&fake_timeline)>=info->pValues[0])return VK_SUCCESS;
        struct timespec t={.tv_sec=0,.tv_nsec=200000};nanosleep(&t,NULL);
    }
    return VK_TIMEOUT;
}
static VkResult fake_counter(VkDevice dev,VkSemaphore sem,uint64_t *value) {
    (void)dev;(void)sem;*value=atomic_load(&fake_timeline);return VK_SUCCESS;
}
static int count_token(FILE *f,const char *word) {
    rewind(f); char line[2048];int hits=0;
    while(fgets(line,sizeof(line),f))if(strstr(line,word))++hits;
    return hits;
}
int main(void) {
    FILE *log=tmpfile();assert(log);
    timeline_watch *w=tw_start((VkDevice)1,1,fake_wait,fake_counter,log,123,7);assert(w);
    /* 2x: renderer waits for V+1 but actual output signals V+2. */
    uint64_t now=tw_now();
    tw_register(w,7,1,0,8,9,now+4000000ull,0);
    tw_submitted(w,7,1);
    tw_deadline(w,7,0,now+5000000ull);
    struct timespec t={.tv_sec=0,.tv_nsec=10000000};nanosleep(&t,NULL);
    atomic_store(&fake_timeline,9);
    nanosleep(&t,NULL);
    /* A failed submit must not be reported as a pending GPU stall. */
    tw_register(w,10,2,0,11,12,tw_now()+10000000ull,0);
    tw_submitted(w,10,0);
    /* No semaphore signal: a bounded unresolved observation. */
    tw_register(w,13,3,0,14,15,tw_now()+10000000ull,1);
    tw_submitted(w,13,1);
    t.tv_nsec=250000000;nanosleep(&t,NULL);
    tw_register(w,16,4,0,17,18,tw_now()+10000000ull,0);
    tw_submitted(w,16,1);
    tw_stop(w);
    assert(count_token(log,"\"watch_end_reason\":\"threshold_reached\"")==1);
    assert(count_token(log,"\"wait_threshold\":8,\"actual_signal_value\":9")==1);
    assert(count_token(log,"\"watch_end_reason\":\"submit_failed\"")==1);
    assert(count_token(log,"\"watch_end_reason\":\"expired\"")==1);
    assert(count_token(log,"\"watch_end_reason\":\"teardown\"")==1);
    assert(count_token(log,"\"event\":\"timeline_watch_summary\"")==1);
    assert(count_token(log,"\"pid\":123,\"generation\":7")==5);
    fclose(log);
    puts("GSF timeline watcher: signal, mismatched wait, failed submit, timeout, teardown PASS");
    return 0;
}

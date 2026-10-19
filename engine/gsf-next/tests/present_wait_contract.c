/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host synchronization contract; does not simulate physical presentation. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../src/swapchain_threads.c"

static uint32_t driver_calls;
static uint64_t driver_timeout;
void *afmf_sc_worker_main(void *arg) { return arg; }
void *afmf_sc_presenter_main(void *arg) { return arg; }
void afmf_log(enum afmf_log_level level, const char *fmt, ...)
{ (void)level; (void)fmt; }
static VKAPI_ATTR VkResult VKAPI_CALL driver_wait(VkDevice device,VkSwapchainKHR swapchain,uint64_t id,uint64_t timeout)
{
    (void)device;(void)swapchain;assert(id==7);driver_calls++;driver_timeout=timeout;return VK_SUCCESS;
}
static void *acknowledge(void *arg)
{
    struct afmf_swapchain *sc=arg;
    struct timespec pause={.tv_nsec=5000000};(void)nanosleep(&pause,NULL);
    pthread_mutex_lock(&sc->job_lock);
    sc->last_submitted_present_id=7;sc->in_flight=0;
    pthread_cond_broadcast(&sc->job_cond);
    pthread_mutex_unlock(&sc->job_lock);return NULL;
}
static void *fail_pending(void *arg)
{
    struct afmf_swapchain *sc=arg;
    struct timespec pause={.tv_nsec=5000000};(void)nanosleep(&pause,NULL);
    pthread_mutex_lock(&sc->job_lock);
    sc->worker_failed=true;sc->deferred_result=VK_ERROR_DEVICE_LOST;sc->in_flight=0;
    pthread_cond_broadcast(&sc->job_cond);pthread_mutex_unlock(&sc->job_lock);return NULL;
}
int main(void)
{
    struct afmf_device dev={0};struct afmf_swapchain sc={0};
    pthread_mutex_init(&dev.lock,NULL);pthread_mutex_init(&sc.job_lock,NULL);
    pthread_mutex_init(&sc.wsi_turn,NULL);pthread_mutex_init(&sc.wsi_lock,NULL);
    pthread_condattr_t attr;pthread_condattr_init(&attr);pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    pthread_cond_init(&sc.job_cond,&attr);pthread_condattr_destroy(&attr);
    sc.handle=(VkSwapchainKHR)(uintptr_t)1;sc.in_flight=1;dev.swapchains=&sc;dev.fns.wait_present=driver_wait;
    assert(afmf_swapchain_wait_present(&dev,sc.handle,7,0,NULL)==VK_TIMEOUT);assert(driver_calls==0);
    assert(afmf_swapchain_wait_present(&dev,sc.handle,7,2000000,NULL)==VK_TIMEOUT);assert(driver_calls==0);
    pthread_t producer;assert(pthread_create(&producer,NULL,acknowledge,&sc)==0);
    assert(afmf_swapchain_wait_present(&dev,sc.handle,7,1000000000,NULL)==VK_SUCCESS);
    assert(driver_calls==1 && driver_timeout>0 && driver_timeout<1000000000);pthread_join(producer,NULL);
    assert(afmf_swapchain_wait_present(&dev,sc.handle,7,0,NULL)==VK_SUCCESS);assert(driver_timeout==0);
    sc.in_flight=1;
    assert(pthread_create(&producer,NULL,fail_pending,&sc)==0);
    assert(afmf_swapchain_wait_present(&dev,sc.handle,8,UINT64_MAX,NULL)==VK_ERROR_DEVICE_LOST);
    pthread_join(producer,NULL);
    assert(afmf_swapchain_wait_present(&dev,sc.handle,8,UINT64_MAX,NULL)==VK_ERROR_DEVICE_LOST);assert(driver_calls==2);
    pthread_cond_destroy(&sc.job_cond);pthread_mutex_destroy(&sc.job_lock);
    pthread_mutex_destroy(&sc.wsi_lock);pthread_mutex_destroy(&sc.wsi_turn);pthread_mutex_destroy(&dev.lock);
    puts("present-wait handoff: PASS (zero/bounded/infinite timeout, remaining budget, device error)");
}

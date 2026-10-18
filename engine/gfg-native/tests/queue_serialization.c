/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual dispatch wrappers resolved through GIPA/GDPA, concurrent producers.
 * The fake lower driver rejects overlapping host access to the same queue.
 */
#define _GNU_SOURCE
#include "../src/layer.c"
#include <assert.h>
#include <stdatomic.h>

static atomic_int inside, calls;
static void lower_enter(void) {
    assert(atomic_fetch_add(&inside,1)==0);
    struct timespec t={.tv_nsec=100000}; nanosleep(&t,NULL);
    atomic_fetch_add(&calls,1); assert(atomic_fetch_sub(&inside,1)==1);
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_submit(VkQueue q,uint32_t n,const VkSubmitInfo *p,VkFence f) {
    (void)q;(void)n;(void)p;(void)f;lower_enter();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_submit2(VkQueue q,uint32_t n,const VkSubmitInfo2 *p,VkFence f) {
    (void)q;(void)n;(void)p;(void)f;lower_enter();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_bind(VkQueue q,uint32_t n,const VkBindSparseInfo *p,VkFence f) {
    (void)q;(void)n;(void)p;(void)f;lower_enter();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_present(VkQueue q,const VkPresentInfoKHR *p) {
    (void)q;(void)p;lower_enter();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL lower_idle(VkQueue q) { (void)q;lower_enter();return VK_SUCCESS; }
static void *producer(void *queue) {
    for(unsigned i=0;i<20;++i) {
        assert(native_QueueSubmit(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
        assert(native_QueueSubmit2(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
        assert(native_QueueSubmit2KHR(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
        assert(native_QueueBindSparse(queue,0,NULL,VK_NULL_HANDLE)==VK_SUCCESS);
        assert(native_serialized_present(queue,NULL)==VK_SUCCESS);
        assert(native_QueueWaitIdle(queue)==VK_SUCCESS);
    }
    return NULL;
}
int main(void) {
    void *key=(void *)(uintptr_t)123;
    VkQueue queue=(VkQueue)&key; VkDevice device=(VkDevice)&key;
    native_device d={.key=key,.handle=device,.submit=lower_submit,.submit2=lower_submit2,
        .submit2_khr=lower_submit2,.bind_sparse=lower_bind,.present=lower_present,.queue_idle=lower_idle};
    pthread_mutex_init(&d.lock,NULL);pthread_mutex_init(&d.queue_lock,NULL);devices=&d;
    assert(native_gipa((VkInstance)&key,"vkQueueSubmit")==native_gdpa(device,"vkQueueSubmit"));
    assert(native_gipa((VkInstance)&key,"vkQueueSubmit2")==native_gdpa(device,"vkQueueSubmit2"));
    assert(native_gipa((VkInstance)&key,"vkQueueSubmit2KHR")==native_gdpa(device,"vkQueueSubmit2KHR"));
    pthread_t threads[3];
    for(unsigned i=0;i<3;++i) assert(pthread_create(&threads[i],NULL,producer,queue)==0);
    for(unsigned i=0;i<3;++i) pthread_join(threads[i],NULL);
    assert(atomic_load(&calls)==360 && !atomic_load(&inside));devices=NULL;
    pthread_mutex_destroy(&d.queue_lock);pthread_mutex_destroy(&d.lock);
    puts("GFG native: concurrent submit/submit2/KHR/bind/present/idle host serialization + GIPA/GDPA PASS");
    return 0;
}

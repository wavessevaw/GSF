/* SPDX-License-Identifier: MIT
 * Mock Vulkan calls check ownership and GPU command recording order only.
 * No physical throughput or compositor validation is claimed.
 */
#include "owner_alpha_record.h"
#include "layer.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

struct afmf_framegen {
    uint32_t slots,sources,targets;
    int direct,batched,ingest;
};
static uint32_t begin_count,end_count,prepare_count,ingest_count,reconstruct_count,finish_count,barriers;
static VkImage snapshot;
static uint32_t invalid_source_writes;
static int expected_direct;
static pthread_mutex_t *checked_queue_lock;
static void clear(void) {
    begin_count=end_count=prepare_count=ingest_count=reconstruct_count=finish_count=barriers=0;
    invalid_source_writes=0;
}
static VkResult VKAPI_PTR cmd_begin(VkCommandBuffer cb,const VkCommandBufferBeginInfo *info) {
    assert(cb && info && info->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    if(checked_queue_lock) {
        /* Recording cannot hold the shared game VkQueue mutex. */
        assert(pthread_mutex_trylock(checked_queue_lock)==0);
        pthread_mutex_unlock(checked_queue_lock);
    }
    begin_count++;return VK_SUCCESS;
}
static VkResult VKAPI_PTR cmd_end(VkCommandBuffer cb) {
    assert(cb);end_count++;return VK_SUCCESS;
}
static void VKAPI_PTR cmd_barrier(VkCommandBuffer cb,VkPipelineStageFlags src,
    VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t nmem,
    const VkMemoryBarrier *mem,uint32_t nbuf,const VkBufferMemoryBarrier *buf,
    uint32_t nimg,const VkImageMemoryBarrier *img) {
    (void)src;(void)dst;(void)flags;(void)nmem;(void)mem;(void)nbuf;(void)buf;
    assert(cb && nimg==1 && img);
    if(img->image==snapshot)invalid_source_writes++;
    const VkImageLayout written=expected_direct?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    assert((img->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && img->newLayout==written)||
           (img->oldLayout==written && img->newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
    barriers++;
}
uint32_t afmf_framegen_owner_slots(const struct afmf_framegen *f){return f->slots;}
uint32_t afmf_framegen_owner_sources(const struct afmf_framegen *f){return f->sources;}
uint32_t afmf_framegen_owner_targets(const struct afmf_framegen *f){return f->targets;}
bool afmf_framegen_direct(const struct afmf_framegen *f){return f->direct!=0;}
bool afmf_framegen_batched(const struct afmf_framegen *f){return f->batched!=0;}
bool afmf_framegen_ingest_direct(const struct afmf_framegen *f){return f->ingest!=0;}
void afmf_framegen_prepare(struct afmf_device *d,struct afmf_framegen *f,uint32_t slot,uint32_t n) {
    (void)d;(void)f;(void)n;assert(slot<4);prepare_count++;
}
void afmf_framegen_record_ingest(struct afmf_device *d,struct afmf_framegen *f,
    VkCommandBuffer cb,uint32_t slot,VkImage current,uint32_t current_index,
    const VkImage *targets,const uint32_t *indices,const float *phases,uint32_t count) {
    (void)d;(void)f;(void)cb;(void)slot;(void)current_index;(void)targets;
    (void)indices;(void)phases;(void)count;
    assert(current==snapshot && begin_count==1 && end_count==0 && prepare_count==1);
    ingest_count++;
}
void afmf_framegen_record_reconstruct(struct afmf_device *d,struct afmf_framegen *f,
    VkCommandBuffer cb,uint32_t slot,VkImage current,uint32_t index,
    const VkImage *targets,const uint32_t *indices,const float *phases,uint32_t count) {
    (void)d;(void)f;(void)cb;(void)slot;(void)index;(void)targets;
    (void)indices;(void)phases;(void)count;
    assert(current==snapshot && begin_count==2 && end_count==1 && ingest_count==1);
    reconstruct_count++;
}
void afmf_framegen_finish(struct afmf_device *d,struct afmf_framegen *f,VkCommandBuffer cb,uint32_t slot) {
    (void)d;(void)f;(void)cb;(void)slot;
    assert(reconstruct_count==1);finish_count++;
}
static unsigned submit_calls;
static VkResult submit_second_failure;
static VkResult VKAPI_PTR test_reset(VkDevice device,uint32_t count,const VkFence *fences) {
    (void)device;assert(count==1 && fences && *fences);return VK_SUCCESS;
}
static VkResult VKAPI_PTR test_submit(VkQueue queue,uint32_t count,
                                     const VkSubmitInfo *info,VkFence fence) {
    assert(queue && count==1 && info && fence);
    assert(info->commandBufferCount==1 && info->waitSemaphoreCount==0);
    assert(info->signalSemaphoreCount==0);
    if(checked_queue_lock)
        assert(pthread_mutex_trylock(checked_queue_lock)==EBUSY);
    ++submit_calls;
    return submit_calls==2?submit_second_failure:VK_SUCCESS;
}
static struct gsf_owner_alpha_pair pair(unsigned count) {
    struct gsf_owner_alpha_pair p={0};
    p.snapshot=(VkImage)(uintptr_t)0x1234u;snapshot=p.snapshot;
    p.layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    p.slot=1;p.source_index=2;p.count=count;
    for(unsigned k=0;k<3;k++) {
        p.targets[k]=(VkImage)(uintptr_t)(0x2345u+k);
        p.indices[k]=3u+k;
        p.phases[k]=(float)(k+1u)/(float)(count+1u);
    }
    return p;
}
int main(void) {
    struct afmf_device d={0};
    d.fns.begin_command_buffer=cmd_begin;
    d.fns.end_command_buffer=cmd_end;
    d.fns.cmd_pipeline_barrier=cmd_barrier;
    struct afmf_framegen f={.slots=4,.sources=4,.targets=12,.direct=1,.batched=1};
    const VkCommandBuffer in=(VkCommandBuffer)(uintptr_t)0x7777u;
    const VkCommandBuffer out=(VkCommandBuffer)(uintptr_t)0x8888u;
    for(unsigned count=0;count<=3;count++) {
        clear();expected_direct=1;
        struct gsf_owner_alpha_pair p=pair(count);
        assert(gsf_owner_alpha_record(&d,&f,in,out,&p)==VK_SUCCESS);
        assert(begin_count==2 && end_count==2 && ingest_count==1 &&
               reconstruct_count==1 && finish_count==1 && prepare_count==1);
        assert(barriers==(count>1?3u:count)+count);
        assert(invalid_source_writes==0);
    }
    f.batched=0;f.direct=0;f.ingest=0;
    for(unsigned count=0;count<=3;count++) {
        clear();expected_direct=0;
        struct gsf_owner_alpha_pair p=pair(count);
        assert(gsf_owner_alpha_record(&d,&f,in,out,&p)==VK_SUCCESS);
        assert(barriers==2u*count && !invalid_source_writes);
    }
    clear();f.direct=1;f.batched=1;
    struct gsf_owner_alpha_pair bad=pair(4);
    assert(gsf_owner_alpha_record(&d,&f,in,out,&bad)==VK_ERROR_INITIALIZATION_FAILED);
    bad=pair(3);bad.layout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    assert(gsf_owner_alpha_record(&d,&f,in,out,&bad)==VK_ERROR_FEATURE_NOT_PRESENT);
    bad=pair(3);bad.phases[1]=bad.phases[0];
    assert(gsf_owner_alpha_record(&d,&f,in,out,&bad)==VK_ERROR_INITIALIZATION_FAILED);
    bad=pair(3);bad.source_index=4u;
    assert(gsf_owner_alpha_record(&d,&f,in,out,&bad)==VK_ERROR_INITIALIZATION_FAILED);
    bad=pair(3);bad.indices[2]=12u;
    assert(gsf_owner_alpha_record(&d,&f,in,out,&bad)==VK_ERROR_INITIALIZATION_FAILED);
    assert(!begin_count && !end_count && !prepare_count && !barriers);
    d.fns.queue_submit=test_submit;
    d.fns.reset_fences=test_reset;
    f.direct=1;f.batched=1;expected_direct=1;
    struct gsf_owner_alpha_pair ok=pair(2);
    struct gsf_owner_alpha_submit_status state={0};
    clear();submit_calls=0;submit_second_failure=VK_SUCCESS;
    assert(gsf_owner_alpha_submit(&d,&f,(VkQueue)(uintptr_t)0x9999,
        in,out,(VkFence)(uintptr_t)0xa1,(VkFence)(uintptr_t)0xa2,
        &ok,&state)==VK_SUCCESS);
    assert(submit_calls==2 && state.ingest_submitted && state.reconstruct_submitted);
    clear();submit_calls=0;submit_second_failure=VK_ERROR_DEVICE_LOST;
    assert(gsf_owner_alpha_submit(&d,&f,(VkQueue)(uintptr_t)0x9999,
        in,out,(VkFence)(uintptr_t)0xa1,(VkFence)(uintptr_t)0xa2,
        &ok,&state)==VK_ERROR_DEVICE_LOST);
    assert(submit_calls==2 && state.ingest_submitted && !state.reconstruct_submitted);
    clear();submit_calls=0;submit_second_failure=VK_SUCCESS;
    assert(gsf_owner_alpha_submit(&d,&f,(VkQueue)(uintptr_t)0x9999,
        in,out,(VkFence)(uintptr_t)0xa1,(VkFence)(uintptr_t)0xa1,
        &ok,&state)==VK_ERROR_INITIALIZATION_FAILED);
    assert(submit_calls==0 && !state.ingest_submitted);
    pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
    checked_queue_lock=&queue_lock;
    clear();submit_calls=0;submit_second_failure=VK_SUCCESS;
    assert(gsf_owner_alpha_submit_serialized(&d,&f,
        (VkQueue)(uintptr_t)0x9999,in,out,
        (VkFence)(uintptr_t)0xa1,(VkFence)(uintptr_t)0xa2,
        &ok,&state,&queue_lock)==VK_SUCCESS);
    assert(submit_calls==2 && state.ingest_submitted && state.reconstruct_submitted);
    /* A partial submit failure still unlocks the queue for the application. */
    clear();submit_calls=0;submit_second_failure=VK_ERROR_DEVICE_LOST;
    assert(gsf_owner_alpha_submit_serialized(&d,&f,
        (VkQueue)(uintptr_t)0x9999,in,out,
        (VkFence)(uintptr_t)0xa1,(VkFence)(uintptr_t)0xa2,
        &ok,&state,&queue_lock)==VK_ERROR_DEVICE_LOST);
    assert(pthread_mutex_trylock(&queue_lock)==0);
    pthread_mutex_unlock(&queue_lock);
    checked_queue_lock=NULL;
    pthread_mutex_destroy(&queue_lock);
    puts("GSF_ALPHA_QUEUE_LOCK recording_unlocked=1"
         " two_gpu_submits_serialized=1 partial_failure_unlock=1 PASS");
    puts("GSF_ALPHA_OWNER_GPU_SUBMITS two_fences=1"
         " no_game_semaphore_replay=1 partial_failure_handled=1 PASS");
    puts("GSF_ALPHA_OUTPUT_OWNER contract=PASS synthetic=0..3 alpha_only=1"
         " source_untouched=1 output_private=1 physical_scanout=UNVERIFIED");
    return 0;
}

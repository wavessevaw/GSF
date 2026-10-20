/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual Alpha/Output Owner runtime resource registration with mock Vulkan
 * dispatch; not a GPU-pixel or physical Deck scanout test.
 */
#include "owner_alpha_runtime.h"
#include "layer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct afmf_framegen {uint32_t sources,targets,slots;bool direct;};
static struct afmf_framegen generator;
static int alive,pool_created,pool_destroyed,alloc_count,created_fences,destroyed_fences;
static unsigned created,submitted,polls,completed;
static struct gsf_owner_alpha_pair last_pair;
static uint32_t last_count;
static unsigned last_slot;
static VkResult VKAPI_PTR create_pool(VkDevice dev,const VkCommandPoolCreateInfo *ci,
    const VkAllocationCallbacks *alloc,VkCommandPool *pool) {
    (void)dev;(void)alloc;assert(ci->queueFamilyIndex==0);
    *pool=(VkCommandPool)(uintptr_t)0x7777u;pool_created++;return VK_SUCCESS;
}
static void VKAPI_PTR destroy_pool(VkDevice dev,VkCommandPool pool,
     const VkAllocationCallbacks *alloc) {
    (void)dev;(void)alloc;assert(pool==(VkCommandPool)(uintptr_t)0x7777u);
    pool_destroyed++;
}
static VkResult VKAPI_PTR allocate(VkDevice dev,const VkCommandBufferAllocateInfo *ai,
    VkCommandBuffer *buffers) {
    (void)dev;assert(ai->commandBufferCount==2u);
    for(uint32_t i=0;i<2;i++)
        buffers[i]=(VkCommandBuffer)(uintptr_t)(0x1000u+(uintptr_t)alloc_count*2u+i);
    alloc_count++;
    return VK_SUCCESS;
}
static VkResult VKAPI_PTR stamp(VkDevice device,void *handle) {
    (void)device;assert(handle);return VK_SUCCESS;
}
static VkResult VKAPI_PTR create_fence(VkDevice d,const VkFenceCreateInfo *ci,
    const VkAllocationCallbacks *alloc,VkFence *out) {
    (void)d;(void)ci;(void)alloc;
    *out=(VkFence)(uintptr_t)(0x3000u+(uintptr_t)++created_fences);
    return VK_SUCCESS;
}
static void VKAPI_PTR destroy_fence(VkDevice d,VkFence fence,
    const VkAllocationCallbacks *alloc) {
    (void)d;(void)alloc;assert(fence);destroyed_fences++;
}
static VkResult VKAPI_PTR wait_fence(VkDevice d,uint32_t n,
    const VkFence *f,VkBool32 wait,uint64_t timeout) {
    (void)d;(void)n;(void)f;(void)wait;(void)timeout;return VK_SUCCESS;
}
static VkResult VKAPI_PTR reset_fence(VkDevice d,uint32_t n,
    const VkFence *f) {
    (void)d;(void)n;(void)f;return VK_SUCCESS;
}
static VkResult VKAPI_PTR queue_submit(VkQueue q,uint32_t n,
    const VkSubmitInfo *info,VkFence fence) {
    (void)q;(void)n;(void)info;(void)fence;return VK_SUCCESS;
}
struct afmf_framegen *afmf_framegen_create(struct afmf_device *dev,VkFormat format,
    VkExtent2D extent,uint32_t slots,const VkImage *images,uint32_t sources,
    const VkImage *targets,uint32_t count,bool ingest,const struct afmf_config *cfg) {
    (void)dev;(void)format;(void)cfg;
    assert(extent.width==800u && extent.height==600u);
    assert(images && targets && sources==4u && count==6u && slots==2u && ingest);
    generator=(struct afmf_framegen){.sources=sources,.targets=count,.slots=slots,.direct=true};
    alive++;created++;return &generator;
}
void afmf_framegen_destroy(struct afmf_device *dev,struct afmf_framegen *fg) {
    (void)dev;assert(fg==&generator);alive--;
}
bool afmf_framegen_ingest_direct(const struct afmf_framegen *fg){return fg->direct;}
void afmf_framegen_set_family(struct afmf_device *dev,struct afmf_framegen *fg,uint32_t f) {
    (void)dev;assert(fg==&generator && f==0u);
}
bool gsf_alpha_owner_transport_init(struct gsf_alpha_owner_transport *t,
    struct afmf_device *dev,struct afmf_framegen *fg,VkQueue q,
    uint32_t slots,const struct gsf_alpha_owner_callbacks *cb,void *user) {
    (void)cb;(void)user;
    assert(fg==&generator && slots==2u && q);
    memset(t,0,sizeof(*t));t->device=dev;t->capacity=slots;t->fault=VK_SUCCESS;return true;
}
VkResult gsf_alpha_owner_enqueue(struct gsf_alpha_owner_transport *t,
    uint64_t frame,uint64_t deadline,uint32_t slot,VkCommandBuffer a,VkCommandBuffer b,
    VkFence c,VkFence d,const struct gsf_owner_alpha_pair *job) {
    assert(t && frame>0 && deadline>0 && a && b && c && d);
    submitted++;last_pair=*job;last_count=job->count;last_slot=slot;return VK_SUCCESS;
}
VkResult gsf_alpha_owner_poll(struct gsf_alpha_owner_transport *t,uint64_t ns) {
    assert(t && ns);polls++;return VK_SUCCESS;
}
bool gsf_alpha_owner_output_done(struct gsf_alpha_owner_transport *t,uint32_t slot,
    uint64_t frame,VkFence done) {
    assert(t && frame && done && slot<2u);completed++;return true;
}
uint32_t gsf_alpha_owner_live(const struct gsf_alpha_owner_transport *t) {
    (void)t;return 0;
}
static bool release(void *u,uint64_t id,uint32_t slot) {
    (void)u;(void)id;(void)slot;return true;
}
static bool ready(void *u,uint64_t id,uint32_t slot,const VkImage *p,uint32_t c) {
    (void)u;(void)id;(void)slot;(void)p;(void)c;return true;
}
static void drop(void *u,uint64_t id,uint32_t slot) {(void)u;(void)id;(void)slot;}
int main(void) {
    VkQueueFamilyProperties family={.queueFlags=VK_QUEUE_COMPUTE_BIT};
    struct afmf_device dev={0};
    dev.queue_family_count=1;dev.queue_families=&family;
    dev.set_loader_data=stamp;
    dev.fns.create_command_pool=create_pool;
    dev.fns.destroy_command_pool=destroy_pool;
    dev.fns.allocate_command_buffers=allocate;
    dev.fns.create_fence=create_fence;
    dev.fns.destroy_fence=destroy_fence;
    dev.fns.wait_for_fences=wait_fence;
    dev.fns.reset_fences=reset_fence;
    dev.fns.queue_submit=queue_submit;
    dev.handle=(VkDevice)(uintptr_t)0x20u;
    VkImage sources[4],targets[6];
    for(unsigned i=0;i<4;i++)sources[i]=(VkImage)(uintptr_t)(0x10000u+i);
    for(unsigned i=0;i<6;i++)targets[i]=(VkImage)(uintptr_t)(0x20000u+i);
    const struct gsf_alpha_owner_callbacks cb={release,ready,drop};
    const struct afmf_config cfg={.enabled=true,.multiplier=4};
    pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    struct gsf_owner_alpha_runtime rt={0};
    assert(gsf_owner_alpha_runtime_init(&rt,&dev,(VkQueue)(uintptr_t)0x88u,
        &lock,0,VK_FORMAT_R8G8B8A8_UNORM,(VkExtent2D){800,600},
        sources,4,targets,6,2,true,&cfg,&cb,NULL)==VK_SUCCESS);
    assert(alive==1 && created==1 && pool_created==1);
    assert(alloc_count==2 && created_fences==4 && rt.started);
    assert(rt.transport.capacity==2);
    for(uint32_t slot=0;slot<2;slot++)for(uint32_t count=0;count<=3;count++) {
        assert(gsf_owner_alpha_runtime_submit(&rt,1u+count+slot*4u,
            10000000u,slot,slot,count)==VK_SUCCESS);
        assert(last_pair.snapshot==sources[slot]);
        assert(last_pair.source_index==slot && last_pair.slot==slot);
        assert(last_slot==slot && last_count==count);
        for(uint32_t i=0;i<3;i++) {
            assert(last_pair.targets[i]==targets[slot*3u+i]);
            assert(last_pair.indices[i]==slot*3u+i);
        }
        for(uint32_t i=0;i<count;i++)
            assert(last_pair.phases[i] > 0.0f && last_pair.phases[i] < 1.0f);
    }
    assert(submitted==8u);
    assert(gsf_owner_alpha_runtime_poll(&rt,20000000u)==VK_SUCCESS && polls==1);
    assert(gsf_owner_alpha_runtime_output_done(&rt,1,99,
        (VkFence)(uintptr_t)0x1234u));
    assert(completed==1u);
    assert(gsf_owner_alpha_runtime_destroy(&rt));
    assert(!alive && pool_created==pool_destroyed && created_fences==destroyed_fences);
    /* Reject image aliasing BEFORE touching Vulkan. */
    targets[0]=sources[0];
    assert(gsf_owner_alpha_runtime_init(&rt,&dev,(VkQueue)(uintptr_t)0x88u,
        &lock,0,VK_FORMAT_R8G8B8A8_UNORM,(VkExtent2D){800,600},
        sources,4,targets,6,2,true,&cfg,&cb,NULL)==VK_ERROR_INITIALIZATION_FAILED);
    assert(created==1);
    pthread_mutex_destroy(&lock);
    puts("GSF_ALPHA_RUNTIME one_device=1 alpha_context=1 actual_slots=2"
         " private_snapshots=4 private_targets=6 mutex=1 no_wsi=1"
         " real_gpu_driver=UNVERIFIED PASS");
    return 0;
}

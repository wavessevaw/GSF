/* SPDX-License-Identifier: MIT
 * Deterministic 12k-frame regression of Alpha -> Output Owner GPU lifecycle.
 * All fake fences are explicitly signalled by the test; no GPU/WSI claim.
 */
#include "owner_alpha_transport.h"
#include "layer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct afmf_framegen { unsigned slots; };
static bool ready_flags[16];
static uint64_t released,ready_outputs,drops;
static uint64_t last_release_frame,last_ready_frame;
static bool force_reconstruct_failure;
static unsigned submitted;
static unsigned queried;
uint32_t afmf_framegen_owner_slots(const struct afmf_framegen *g) {return g->slots;}

static VkResult VKAPI_PTR wait_probe(VkDevice device,uint32_t count,
                        const VkFence *fences,VkBool32 all,uint64_t timeout) {
    (void)device;
    assert(count==1 && fences && all==VK_TRUE && timeout==0u);
    queried++;
    const uintptr_t id=(uintptr_t)fences[0];
    assert(id>=1u && id<=16u);
    return ready_flags[id-1]?VK_SUCCESS:VK_TIMEOUT;
}
VkResult gsf_owner_alpha_submit(struct afmf_device *dev,struct afmf_framegen *fg,
    VkQueue queue,VkCommandBuffer ingest_cmd,VkCommandBuffer reconstruct_cmd,
    VkFence ingest_done,VkFence reconstruct_done,
    const struct gsf_owner_alpha_pair *job,
    struct gsf_owner_alpha_submit_status *status)
{
    (void)dev;(void)fg;
    assert(queue && ingest_cmd && reconstruct_cmd && ingest_done && reconstruct_done);
    assert(job && status && job->slot<2u);
    submitted++;
    status->ingest_submitted=true;
    if(force_reconstruct_failure) {
        status->reconstruct_submitted=false;
        return VK_ERROR_DEVICE_LOST;
    }
    status->reconstruct_submitted=true;
    return VK_SUCCESS;
}
static bool release_source(void *user,uint64_t frame,uint32_t slot) {
    assert(user==(void*)(uintptr_t)0x9999u && frame>last_release_frame && slot<2);
    last_release_frame=frame;released++;return true;
}
static bool synthetic_ready(void *user,uint64_t frame,uint32_t slot,
                            const VkImage *images,uint32_t count) {
    assert(user==(void*)(uintptr_t)0x9999u && frame>last_ready_frame && slot<2);
    assert(count>0 && count<=3 && images && images[0]);
    last_ready_frame=frame;ready_outputs++;return true;
}
static void synthetic_dropped(void *user,uint64_t frame,uint32_t slot) {
    assert(user==(void*)(uintptr_t)0x9999u && frame && slot<2);drops++;
}
static struct gsf_owner_alpha_pair job(unsigned slot,unsigned count) {
    struct gsf_owner_alpha_pair p={0};
    p.slot=slot;p.count=count;
    p.snapshot=(VkImage)(uintptr_t)(0x100+slot);
    for(unsigned k=0;k<count;k++)
        p.targets[k]=(VkImage)(uintptr_t)(0x200+3u*slot+k);
    return p;
}
static VkFence ingest(unsigned slot) {
    return (VkFence)(uintptr_t)(slot*2u+1u);
}
static VkFence reconstruct(unsigned slot) {
    return (VkFence)(uintptr_t)(slot*2u+2u);
}
static VkFence output_fence(unsigned slot) {
    return (VkFence)(uintptr_t)(slot+5u);
}
static VkCommandBuffer command(unsigned slot,unsigned i) {
    return (VkCommandBuffer)(uintptr_t)(slot*2u+i+1u);
}
int main(void) {
    struct afmf_device d={0};
    d.fns.wait_for_fences=wait_probe;
    struct afmf_framegen fg={.slots=2};
    const struct gsf_alpha_owner_callbacks cb={
        .release_source=release_source,
        .synthetic_ready=synthetic_ready,
        .synthetic_dropped=synthetic_dropped};
    struct gsf_alpha_owner_transport tr={0};
    assert(gsf_alpha_owner_transport_init(&tr,&d,&fg,
           (VkQueue)(uintptr_t)0x9999u,2,&cb,(void*)(uintptr_t)0x9999u));
    assert(tr.capacity==2 && tr.fault==VK_SUCCESS);

    /* Release each source after ingest GPU completion, while the optional
     * reconstruction still runs and the output slot must stay pinned. */
    for(uint64_t frame=1;frame<=12000;frame++) {
        const unsigned slot=(unsigned)(frame&1u);
        const unsigned count=frame%7u==0u?0u:(unsigned)(frame%3u)+1u;
        struct gsf_owner_alpha_pair p=job(slot,count);
        const uint64_t t0=1000000000ull+frame*30000000ull;
        const uint64_t deadline=t0+10000000ull;
        ready_flags[(uintptr_t)ingest(slot)-1]=false;
        ready_flags[(uintptr_t)reconstruct(slot)-1]=false;
        ready_flags[(uintptr_t)output_fence(slot)-1]=false;
        assert(gsf_alpha_owner_enqueue(&tr,frame,deadline,slot,
            command(slot,0),command(slot,1),ingest(slot),
            reconstruct(slot),&p)==VK_SUCCESS);
        assert(gsf_alpha_owner_live(&tr)==1u);
        const uint64_t previous= released;
        assert(gsf_alpha_owner_poll(&tr,t0+1000000ull)==VK_SUCCESS);
        assert(released==previous); /* submitted != GPU ready */
        ready_flags[(uintptr_t)ingest(slot)-1]=true;
        assert(gsf_alpha_owner_poll(&tr,t0+2000000ull)==VK_SUCCESS);
        assert(released==previous+1); /* app/source snapshot freed early */
        assert(gsf_alpha_owner_live(&tr)==1);
        assert(!gsf_alpha_owner_output_done(&tr,slot,frame,output_fence(slot))); /* recon not ready */
        ready_flags[(uintptr_t)reconstruct(slot)-1]=true;
        if(frame%5u==0) {
            /* Artificially late optional output is discarded BEFORE WSI. */
            assert(gsf_alpha_owner_poll(&tr,deadline)==VK_SUCCESS);
        } else {
            assert(gsf_alpha_owner_poll(&tr,t0+3000000ull)==VK_SUCCESS);
            if(count) {
                assert(!gsf_alpha_owner_output_done(&tr,slot,frame+1,output_fence(slot)));
                assert(!gsf_alpha_owner_output_done(&tr,slot,frame,output_fence(slot)));
                ready_flags[(uintptr_t)output_fence(slot)-1]=true;
                assert(gsf_alpha_owner_output_done(&tr,slot,frame,output_fence(slot)));
                assert(!gsf_alpha_owner_output_done(&tr,slot,frame,output_fence(slot)));
            }
        }
        assert(gsf_alpha_owner_live(&tr)==0u);
        assert(tr.fault==VK_SUCCESS);
    }
    assert(released==12000u && tr.ingested==12000u);
    assert(tr.generated==ready_outputs && tr.dropped==drops);
    assert(tr.generated>0 && tr.dropped>0 && tr.retired==ready_outputs);
    assert(submitted==12000u && queried>=36000u);

    /* A source cannot be replaced while synthetic GPU output is in flight
     * nor after handing output off until owner ACKs its retirement. */
    const uint64_t frame=12001u,t0=500000000000ull;
    struct gsf_owner_alpha_pair p=job(0u,2u);
    ready_flags[0]=ready_flags[1]=false;
    ready_flags[(uintptr_t)output_fence(0)-1]=false;
    assert(gsf_alpha_owner_enqueue(&tr,frame,t0+30000000ull,0,
        command(0,0),command(0,1),ingest(0),reconstruct(0),&p)==VK_SUCCESS);
    assert(gsf_alpha_owner_enqueue(&tr,frame+1u,t0+40000000ull,0,
        command(0,0),command(0,1),ingest(0),reconstruct(0),&p)==VK_NOT_READY);
    ready_flags[0]=true;
    assert(gsf_alpha_owner_poll(&tr,t0+2000000ull)==VK_SUCCESS);
    ready_flags[1]=true;
    assert(gsf_alpha_owner_poll(&tr,t0+4000000ull)==VK_SUCCESS);
    assert(gsf_alpha_owner_live(&tr)==1);
    assert(!gsf_alpha_owner_output_done(&tr,0,frame,output_fence(0)));
    ready_flags[(uintptr_t)output_fence(0)-1]=true;
    assert(gsf_alpha_owner_output_done(&tr,0,frame,output_fence(0)));
    assert(gsf_alpha_owner_live(&tr)==0);

    /* Partial submit error must never free or recycle unretired GPU work. */
    force_reconstruct_failure=true;
    p=job(0,2);ready_flags[0]=false;ready_flags[1]=false;
    assert(gsf_alpha_owner_enqueue(&tr,frame+2u,t0+50000000ull,0,
        command(0,0),command(0,1),ingest(0),reconstruct(0),&p)==VK_ERROR_DEVICE_LOST);
    assert(tr.active[0].ingest_submitted && !tr.active[0].reconstruct_submitted);
    assert(gsf_alpha_owner_live(&tr)==1);
    ready_flags[0]=true;
    assert(gsf_alpha_owner_poll(&tr,t0+5000000ull)==VK_ERROR_DEVICE_LOST);
    assert(gsf_alpha_owner_live(&tr)==1);
    assert(tr.active[0].source_released);
    assert(tr.gpu_errors==1);
    puts("GSF_ALPHA_OWNER_TRANSPORT 12000_frames=PASS"
         " real_source_released_before_synth=1"
         " bounded_slots=1 late_synth_culled=1"
         " no_early_reuse=1 output_copy_fence_required=1"
         " partial_gpu_failure_quarantined=1"
         " wsi_calls=0 physical_fps=UNVERIFIED");
    return 0;
}

/* SPDX-License-Identifier: MIT */
#include "owner_alpha_transport.h"
#include <string.h>

static VkResult fence_ready(struct gsf_alpha_owner_transport *t,
                            VkFence fence,bool *ready)
{
    *ready=false;
    if(!fence)return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r=t->device->fns.wait_for_fences(
        t->device->handle,1,&fence,VK_TRUE,0u);
    if(r==VK_SUCCESS)*ready=true;
    return r==VK_TIMEOUT?VK_SUCCESS:r;
}

bool gsf_alpha_owner_transport_init(struct gsf_alpha_owner_transport *t,
    struct afmf_device *device,struct afmf_framegen *generator,VkQueue compute,
    uint32_t capacity,const struct gsf_alpha_owner_callbacks *events,void *user)
{
    if(!t||!device||!generator||!compute||!events||
       !events->release_source||!events->synthetic_ready||
       !events->synthetic_dropped||!device->fns.wait_for_fences||
       !capacity||capacity>GSF_ALPHA_OWNER_MAX_SLOTS||
       capacity>afmf_framegen_owner_slots(generator))
        return false;
    memset(t,0,sizeof(*t));
    t->device=device;t->generator=generator;t->compute_queue=compute;
    t->capacity=capacity;t->events=*events;t->user=user;
    t->fault=VK_SUCCESS;
    return true;
}

VkResult gsf_alpha_owner_enqueue(struct gsf_alpha_owner_transport *t,
    uint64_t frame_id,uint64_t deadline_ns,uint32_t slot,
    VkCommandBuffer ingest_cmd,VkCommandBuffer reconstruct_cmd,
    VkFence ingest_done,VkFence reconstruct_done,
    const struct gsf_owner_alpha_pair *job)
{
    if(!t||!frame_id||!job||slot>=t->capacity||
       job->slot!=slot||!ingest_done||!reconstruct_done||
       frame_id<=t->last_frame_id)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(t->fault!=VK_SUCCESS)return t->fault;
    struct gsf_alpha_owner_record *s=&t->active[slot];
    if(s->frame_id)return VK_NOT_READY; /* no guessing about output retirement */
    if(job->count && !deadline_ns)return VK_ERROR_INITIALIZATION_FAILED;

    /* Token allocated before submissions; partial failure stays pinned until
     * the corresponding submitted fence is signaled and caller shuts down. */
    s->frame_id=frame_id;s->deadline_ns=deadline_ns;s->slot=slot;
    s->count=job->count;s->ingest_done=ingest_done;
    s->reconstruct_done=reconstruct_done;
    for(uint32_t i=0;i<job->count && i<GSF_OWNER_ALPHA_OUTPUTS;i++)
        s->targets[i]=job->targets[i];
    t->last_frame_id=frame_id;
    struct gsf_owner_alpha_submit_status status={0};
    VkResult result=gsf_owner_alpha_submit_serialized(t->device,t->generator,
        t->compute_queue,ingest_cmd,reconstruct_cmd,
        ingest_done,reconstruct_done,job,&status,t->submit_lock);
    s->ingest_submitted=status.ingest_submitted;
    s->reconstruct_submitted=status.reconstruct_submitted;
    if(result!=VK_SUCCESS) {
        t->fault=result;
        ++t->gpu_errors;
        /* Source is not safe to release unless ingest was submitted AND
         * completion is actually observed; never drop the occupied token. */
    }
    return result;
}

VkResult gsf_alpha_owner_poll(struct gsf_alpha_owner_transport *t,uint64_t now_ns)
{
    if(!t||!now_ns)return VK_ERROR_INITIALIZATION_FAILED;
    for(uint32_t i=0;i<t->capacity;i++) {
        struct gsf_alpha_owner_record *s=&t->active[i];
        if(!s->frame_id)continue;
        if(s->ingest_submitted && !s->source_released) {
            bool ready=false;
            VkResult r=fence_ready(t,s->ingest_done,&ready);
            if(r!=VK_SUCCESS) {
                t->fault=r;++t->gpu_errors;return r;
            }
            if(ready) {
                if(!t->events.release_source(t->user,s->frame_id,i)) {
                    t->fault=VK_ERROR_UNKNOWN;return t->fault;
                }
                s->source_released=true;++t->ingested;
            }
        }
        if(s->reconstruct_submitted && !s->output_handed_off && !s->dropped) {
            bool ready=false;
            VkResult r=fence_ready(t,s->reconstruct_done,&ready);
            if(r!=VK_SUCCESS) {
                t->fault=r;++t->gpu_errors;return r;
            }
            if(ready) {
                /* At or after deadline we cannot issue a fresh synthetic
                 * present. The owner still presents real source separately. */
                if(s->count==0 || now_ns>=s->deadline_ns) {
                    t->events.synthetic_dropped(t->user,s->frame_id,i);
                    s->dropped=true;++t->dropped;
                    s->output_retired=true;
                } else {
                    if(!t->events.synthetic_ready(t->user,s->frame_id,i,
                                                  s->targets,s->count)) {
                        t->fault=VK_ERROR_UNKNOWN;return t->fault;
                    }
                    s->output_handed_off=true;++t->generated;
                }
            }
        }
        /* No GPU resource is recycled while an already submitted stage
         * is still in flight or an owner output copy still owns the images. */
        if(s->source_released && s->output_retired)
            *s=(struct gsf_alpha_owner_record){0};
    }
    return t->fault;
}

bool gsf_alpha_owner_output_done(struct gsf_alpha_owner_transport *t,
    uint32_t slot,uint64_t frame_id,VkFence output_copy_done)
{
    if(!t||slot>=t->capacity||!frame_id||!output_copy_done)return false;
    struct gsf_alpha_owner_record *s=&t->active[slot];
    if(s->frame_id!=frame_id||!s->output_handed_off||
       s->output_retired||!s->source_released)return false;
    bool ready=false;
    const VkResult result=fence_ready(t,output_copy_done,&ready);
    if(result!=VK_SUCCESS) {
        t->fault=result;++t->gpu_errors;return false;
    }
    if(!ready)return false;
    s->output_retired=true;
    ++t->retired;
    /* GPU copy completed; the owner separately manages downstream WSI
     * acquired-image lifetime and compositor retirement. */
    *s=(struct gsf_alpha_owner_record){0};
    return true;
}

uint32_t gsf_alpha_owner_live(const struct gsf_alpha_owner_transport *t)
{
    uint32_t count=0;
    if(t)for(uint32_t i=0;i<t->capacity;i++)
        count+=t->active[i].frame_id!=0;
    return count;
}

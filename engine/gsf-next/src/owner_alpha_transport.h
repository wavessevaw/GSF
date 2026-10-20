/* SPDX-License-Identifier: MIT
 * GSF Next Alpha private GPU producer / independent Output Owner lifecycle.
 * No game/WSI acquire or vkQueuePresentKHR is allowed in this component.
 *
 * Owner provides pinned private source and target VkImages, Vulkan command
 * buffers, fences and a queue. Owner also serializes VkQueue host operations.
 * Alpha reports source ingest completion and optional output readiness
 * separately. Owner alone decides how/when to present and MUST ACK output
 * retirement before a work slot can be reused.
 */
#pragma once

#include "owner_alpha_record.h"
#include <stdbool.h>
#include <stdint.h>

#define GSF_ALPHA_OWNER_MAX_SLOTS 8u

struct gsf_alpha_owner_record {
    uint64_t frame_id;
    uint64_t deadline_ns; /* absolute; 0 means drop ALL optional output */
    uint32_t slot;
    uint32_t count;
    VkFence ingest_done;
    VkFence reconstruct_done;
    VkImage targets[GSF_OWNER_ALPHA_OUTPUTS];
    bool ingest_submitted;
    bool reconstruct_submitted;
    bool source_released;
    bool output_handed_off;
    bool output_retired;
    bool dropped;
};

struct gsf_alpha_owner_callbacks {
    /* Called once after actual Alpha INGEST fence. Release private snapshot
     * only if this returns true; the real original has its own owner path.
     * No output WSI dependency and no application render semaphore replay. */
    bool (*release_source)(void *user,uint64_t frame_id,uint32_t slot);
    /* Called only for ready, on-time optional outputs, once. False latches
     * a failure: do not reuse GPU slot without verified retirement. */
    bool (*synthetic_ready)(void *user,uint64_t frame_id,uint32_t slot,
                            const VkImage *images,uint32_t count);
    /* Called when the synthetic GPU work completes after its deadline.
     * No images are handed to WSI, and the owner may recycle the slot. */
    void (*synthetic_dropped)(void *user,uint64_t frame_id,uint32_t slot);
};
struct gsf_alpha_owner_transport {
    struct afmf_device *device;
    struct afmf_framegen *generator;
    VkQueue compute_queue;
    uint32_t capacity;
    struct gsf_alpha_owner_callbacks events;
    void *user;
    struct gsf_alpha_owner_record active[GSF_ALPHA_OWNER_MAX_SLOTS];
    uint64_t last_frame_id;
    uint64_t ingested;
    uint64_t generated;
    uint64_t dropped;
    uint64_t retired;
    uint64_t gpu_errors;
    VkResult fault;
};

/* Owner calls only after GPU snapshot copy completes and before target
 * VkImages are handed to any output consumer. */
bool gsf_alpha_owner_transport_init(struct gsf_alpha_owner_transport *t,
    struct afmf_device *device,struct afmf_framegen *generator,VkQueue compute,
    uint32_t capacity,const struct gsf_alpha_owner_callbacks *events,void *user);

/* Records and submits using Alpha's real FidelityFX kernels.
 * On failure after ingest submit, the ingest fence MUST still be reaped:
 * the token stays occupied, the error is latched, no WSI fallback may
 * replay consumed GPU work. */
VkResult gsf_alpha_owner_enqueue(struct gsf_alpha_owner_transport *t,
    uint64_t frame_id,uint64_t deadline_ns,uint32_t slot,
    VkCommandBuffer ingest_cmd,VkCommandBuffer reconstruct_cmd,
    VkFence ingest_done,VkFence reconstruct_done,
    const struct gsf_owner_alpha_pair *job);

/* Owner invokes from its worker with MONOTONIC timestamp; probes fences
 * nonblocking (vkWaitForFences with 0 timeout). Does not wait for WSI.
 * On-time ready frames remain pinned until explicit output_done. */
VkResult gsf_alpha_owner_poll(struct gsf_alpha_owner_transport *t,uint64_t now_ns);

/* Caller guarantees its separate WSI output copy/reconstruction consumers
 * have completely finished GPU reads of these generated images. */
bool gsf_alpha_owner_output_done(struct gsf_alpha_owner_transport *t,
    uint32_t slot,uint64_t frame_id);
uint32_t gsf_alpha_owner_live(const struct gsf_alpha_owner_transport *t);

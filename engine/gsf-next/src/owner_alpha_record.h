/* SPDX-License-Identifier: MIT
 * Alpha generation command recorder for Output Owner private snapshots.
 * The owner owns all GPU submits, fences, output deadlines and real WSI.
 * This unit never links LMC or calls vkQueuePresentKHR.
 */
#pragma once
#include "framegen.h"
#include <stdint.h>

#define GSF_OWNER_ALPHA_OUTPUTS 3u
struct gsf_owner_alpha_pair {
    VkImage snapshot;                /* private GPU image, NOT game WSI */
    VkImageLayout layout;            /* TRANSFER_SRC or GENERAL */
    uint32_t source_index;           /* index registered with Alpha */
    uint32_t slot;                   /* free work slot with retired fence */
    uint32_t count;                  /* 0..3 synthetic outputs */
    VkImage targets[GSF_OWNER_ALPHA_OUTPUTS];
    uint32_t indices[GSF_OWNER_ALPHA_OUTPUTS];
    float phases[GSF_OWNER_ALPHA_OUTPUTS];
};

/* Record Alpha ingest then Alpha OF+reconstruction in separate VkCommandBuffers.
 * Alpha NEVER changes the private snapshot layout. Source can be recycled
 * after the ingest fence, while reconstruction consumes internal Alpha history.
 * Generated targets finish as TRANSFER_SRC_OPTIMAL for Output Owner copy.
 * Both commands must be submitted in order on one queue (external sync).
 * On any failure the owner discards both commands; none are automatically
 * submitted or presented by this recorder.
 */
VkResult gsf_owner_alpha_record(struct afmf_device *dev,
                               struct afmf_framegen *fg,
                               VkCommandBuffer ingest_cmd,
                               VkCommandBuffer reconstruct_cmd,
                               const struct gsf_owner_alpha_pair *job);

/* No application semaphores or WSI handles may be consumed here. Call
 * ONLY after Output Owner's private snapshot copy is GPU-ready.
 * The caller owns VkQueue host synchronization and must not reuse either
 * fence or buffer before retiring its previous use. Separate fences allow
 * real-source release after ingest, independently of synthetic readiness.
 * Partial failure is observable; never replay the submitted ingest. */
struct gsf_owner_alpha_submit_status {
    bool ingest_submitted;
    bool reconstruct_submitted;
};
VkResult gsf_owner_alpha_submit(struct afmf_device *dev,struct afmf_framegen *fg,
    VkQueue queue,VkCommandBuffer ingest_cmd,VkCommandBuffer reconstruct_cmd,
    VkFence ingest_done,VkFence reconstruct_done,
    const struct gsf_owner_alpha_pair *job,
    struct gsf_owner_alpha_submit_status *status);

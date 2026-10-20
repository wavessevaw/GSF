/* SPDX-License-Identifier: GPL-3.0-or-later
 * One-VkDevice runtime bridge: GSF Next Alpha FidelityFX producer embedded
 * inside the Output Owner lifecycle. No Temporal Engine, no LMC, no WSI calls.
 * Output Owner owns source snapshot memory, synthetic textures and presentation.
 */
#pragma once

#include "owner_alpha_transport.h"
#include "config.h"
#include <pthread.h>

#define GSF_OWNER_SOURCES 8u
struct gsf_owner_alpha_runtime {
    struct afmf_device *device;
    struct afmf_framegen *generator;
    struct gsf_alpha_owner_transport transport;
    VkQueue queue;
    pthread_mutex_t *queue_mutex; /* must be the actual queue's host lock */
    pthread_mutex_t record_lock; /* guards Alpha CPU history/commands, not game VkQueue */
    bool record_lock_ready;
    VkCommandPool commands;
    VkCommandBuffer ingest_cmd[GSF_ALPHA_OWNER_MAX_SLOTS];
    VkCommandBuffer reconstruct_cmd[GSF_ALPHA_OWNER_MAX_SLOTS];
    VkFence ingest_done[GSF_ALPHA_OWNER_MAX_SLOTS];
    VkFence reconstruct_done[GSF_ALPHA_OWNER_MAX_SLOTS];
    VkImage snapshots[GSF_OWNER_SOURCES];
    VkImage targets[GSF_ALPHA_OWNER_MAX_SLOTS*GSF_OWNER_ALPHA_OUTPUTS];
    uint32_t snapshot_count,slot_count,target_count,outputs_per_pair;
    bool started;
};

/* Source images are already private GPU snapshots owned by Output Owner,
 * never virtual game VkImages or compositor WSI VkImages. All sources must
 * carry SAMPLED + TRANSFER_SRC and be created for same device/format/extent.
 * Synthetic targets require STORAGE + TRANSFER_SRC (direct) or TRANSFER_DST
 * + TRANSFER_SRC (copy). For multiplier x2 allocate 1 target per slot, x3
 * allocate 2 targets per slot. No x4/dummy target allocation here.
 * No image allocation/WSI ownership transfer here.
 *
 * Only one of the two existing native layers may be installed in a game.
 * This module lives in the Alpha library, not a second interceptor.
 */
VkResult gsf_owner_alpha_runtime_init(
    struct gsf_owner_alpha_runtime *r,
    struct afmf_device *device,VkQueue queue,pthread_mutex_t *queue_mutex,
    uint32_t queue_family,VkFormat format,VkExtent2D extent,
    const VkImage *snapshots,uint32_t snapshot_count,
    const VkImage *targets,uint32_t target_count,uint32_t slots,
    bool direct_ingest,const struct afmf_config *config,
    const struct gsf_alpha_owner_callbacks *callbacks,void *user);

/* Immutable absolute source deadline, no implicit WSI present.
 * The snapshot MUST have been copied and its GPU-copy fence completed;
 * release_source callback releases the Alpha pin only after ingest fence.
 */
VkResult gsf_owner_alpha_runtime_submit(
    struct gsf_owner_alpha_runtime *r,uint64_t frame_id,uint64_t deadline_ns,
    uint32_t slot,uint32_t snapshot_index,uint32_t count);

VkResult gsf_owner_alpha_runtime_poll(struct gsf_owner_alpha_runtime *r,uint64_t now_ns);

/* output_copy_done is the actual GPU fence after the owner has copied the
 * synthetic target(s) to downstream WSI. A returned QueuePresentKHR is not
 * enough. Owner separately retains swapchain WSI image retirement rights. */
bool gsf_owner_alpha_runtime_output_done(
    struct gsf_owner_alpha_runtime *r,uint32_t slot,uint64_t frame_id,
    VkFence output_copy_done);

/* Caller must have drained external owner output and seen ZERO live tokens,
 * including quarantined partial GPU errors. False leaves state untouched.
 * Stop scheduling, retire all external WSI/GPU work before destruction. */
bool gsf_owner_alpha_runtime_destroy(struct gsf_owner_alpha_runtime *r);

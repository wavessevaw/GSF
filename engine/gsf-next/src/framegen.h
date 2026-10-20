#pragma once

#include "layer.h"
#include "renderer_choice.h"

/* Frame generation proper: FidelityFX Optical Flow (colour in, 8x8 block motion + scene change
 * detector out) followed by the layer's own interpolation shader. One instance per swapchain; the
 * pipelines are shared per device and built on first use. */

struct afmf_config;
struct afmf_framegen;

/* Whether the interpolated frame can be written straight into a swapchain image of this format
 * (the image then needs STORAGE usage): the format takes storage writes, and either has a SPIR-V
 * image format or the application enabled shaderStorageImageWriteWithoutFormat. */
VkFormat afmf_framegen_output_format(VkFormat source_format);

bool afmf_framegen_can_write_direct(const struct afmf_device *dev, VkFormat swapchain_format);

/* Source images belong to the game's WSI. Targets are the independent GPU output ring;
 * they need STORAGE usage for direct output. Counts are independent. Unsupported storage
 * formats pass NULL targets and retain the scratch/copy path. direct_ingest requires
 * SAMPLED usage on source images. NULL means generation is unavailable: use real-only. */
struct afmf_framegen *afmf_framegen_create(struct afmf_device *dev, VkFormat swapchain_format,
                                           VkExtent2D extent, uint32_t slots,
                                           const VkImage *images, uint32_t image_count,
                                           const VkImage *targets, uint32_t target_count, bool direct_ingest, const struct afmf_config *config);

/* True when the interpolator writes the target itself: the caller hands it in GENERAL layout. */
bool afmf_framegen_direct(const struct afmf_framegen *fg);
bool afmf_framegen_batched(const struct afmf_framegen *fg);
/* Called after the slot fence, before choosing output layouts. No GPU waits added. */
void afmf_framegen_prepare(struct afmf_device *dev,struct afmf_framegen *fg,uint32_t slot,uint32_t outputs);
void afmf_framegen_finish(struct afmf_device *dev,struct afmf_framegen *fg,VkCommandBuffer cmd,uint32_t slot);

/* Whether the frame can be read straight from a swapchain image of this format into the colour
 * ring and the luma in one pass (the image then needs SAMPLED usage): the swapchain's format is
 * the ring's own (no sRGB decode on the way) and the ring takes storage writes. */
bool afmf_framegen_can_ingest_direct(const struct afmf_device *dev, VkFormat swapchain_format);

/* True when the ingest samples the current image itself: the caller hands it in GENERAL layout
 * instead of TRANSFER_SRC. */
bool afmf_framegen_ingest_direct(const struct afmf_framegen *fg);

void afmf_framegen_destroy(struct afmf_device *dev, struct afmf_framegen *fg);

/* Read-only resource bounds for external, privately owned snapshots.
 * owner_sources returns ALL registered images, including copy-only ingest
 * where source_count for direct descriptors is correctly zero. */
uint32_t afmf_framegen_owner_slots(const struct afmf_framegen *fg);
uint32_t afmf_framegen_owner_sources(const struct afmf_framegen *fg);
uint32_t afmf_framegen_owner_targets(const struct afmf_framegen *fg);

/* The queue family afmf_framegen_record's command buffers belong to, once the caller knows it:
 * the fixed part of a frame is then recorded once per (slot, parity, companion) into secondary
 * command buffers of that family and only executed from then on. Without this call (or when it
 * fails) every frame is recorded in full. */
void afmf_framegen_set_family(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t family);

/* Records, into a command buffer on a compute-capable queue:
 *   1. a copy of `current` (a swapchain image in TRANSFER_SRC_OPTIMAL) into the colour ring,
 *   2. the luma pyramid and scene change detector on the new frame, and, when `target` is not
 *      VK_NULL_HANDLE, the block search,
 *   3. when `target` is not VK_NULL_HANDLE (a swapchain image in TRANSFER_DST_OPTIMAL): the
 *      interpolated frame between the previous and the new one, copied into it.
 * Without a target the frame only enters the history (about a fifth of the cost).
 * `slot` selects the constants region so `slots` command buffers can be in flight;
 * `current_index` selects a source WSI view; `target_index` selects a private output view. */
void afmf_framegen_record_multi(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                          uint32_t slot, VkImage current, uint32_t current_index,
                          const VkImage *targets, const uint32_t *indices, const float *phases, uint32_t count);

/* Two-part recording for experimental early-real output. The first command
 * fully snapshots the game image and restores its PRESENT_SRC layout; the
 * second reads only private images. Must execute in that order on the same
 * compute queue. No generated output can touch the game's original image. */
void afmf_framegen_record_ingest(struct afmf_device *dev,struct afmf_framegen *fg,
                                VkCommandBuffer cmd,uint32_t slot,VkImage current,uint32_t current_index,
                                const VkImage *targets,const uint32_t *indices,const float *phases,uint32_t count);
void afmf_framegen_record_reconstruct(struct afmf_device *dev,struct afmf_framegen *fg,
                                     VkCommandBuffer cmd,uint32_t slot,VkImage current,uint32_t current_index,
                                     const VkImage *targets,const uint32_t *indices,const float *phases,uint32_t count);

void afmf_framegen_record(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                          uint32_t slot, VkImage current, uint32_t current_index, VkImage target,
                          uint32_t target_index);

/* Pyramid levels the search uses from now on, between 5 and what the swapchain was set up with;
 * fewer levels cost less and reach shorter motion. The governor lowers it under GPU contention. */
void afmf_framegen_set_levels(struct afmf_framegen *fg, uint32_t levels);
uint32_t afmf_framegen_max_levels(const struct afmf_framegen *fg);

/* Debug dumps (AFMF_DUMP_DIR): afmf_framegen_record also copies the generated frame, the flow
 * and the luma into a readback buffer while `afmf_framegen_dump_pending` is true (the first
 * four companions from frame 8 on, after the scene change detector's warm-up);
 * `afmf_framegen_dump_recorded` says whether the command buffer just recorded did, and once
 * its submission has completed afmf_framegen_dump_write stores <dir>/afmf_generated_<frame>.ppm
 * and <dir>/afmf_flow_<frame>.txt ("vx vy" per block, prev = cur + v). 8-bit variants only. */
/* Rearms the four-frame dump when AFMF_DUMP_DIR/dump-now exists, so a dump can be taken from
 * whatever is on screen instead of the frames that follow a swapchain's creation. */
void afmf_framegen_dump_arm(struct afmf_framegen *fg);
bool afmf_framegen_dump_pending(const struct afmf_framegen *fg);
bool afmf_framegen_dump_recorded(const struct afmf_framegen *fg);
void afmf_framegen_dump_write(struct afmf_device *dev, struct afmf_framegen *fg);

/* Compiles the per-device pipelines on a thread of their own, from vkCreateDevice; the first
 * swapchain joins it. Frees them from vkDestroyDevice. */
void afmf_framegen_pipelines_prepare(struct afmf_device *dev);
void afmf_framegen_pipelines_destroy(struct afmf_device *dev);

/* Most recently retired pair, GPU timestamps only; -1 when unavailable. */
double afmf_framegen_gpu_ms(const struct afmf_framegen *fg);
enum gsf_renderer afmf_framegen_gpu_renderer(const struct afmf_framegen *fg);
double afmf_framegen_reconstruction_ms(const struct afmf_framegen *fg);
void afmf_framegen_gpu_stages(const struct afmf_framegen *fg, double *motion, double warps[3]);

void afmf_framegen_set_diagnostics(struct afmf_framegen *fg,bool enabled);

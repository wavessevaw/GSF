// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal standalone Vulkan host for the KEDAVRA pass plan. It reproduces the
// part of Gamescope's compute descriptor layout the shaders use (binding 0
// uniform constants, 1 storage target, 3 sampled textures), so tests and
// benchmarks run the same SPIR-V the compositor runs. Used by kfg_vktest
// (correctness against analytic ground truth) and by the on-device benchmark.
#ifndef KFG_VK_H
#define KFG_VK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "kfg_plan.h"

typedef struct kfg_vk_image {
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	VkFormat format;
	uint32_t width, height;
} kfg_vk_image;

typedef struct kfg_vk {
	VkInstance instance;
	VkPhysicalDevice phys;
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd;
	VkFence fence;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout layout;
	VkPipeline pipelines[KFG_SHADER_COUNT];
	VkDescriptorPool dpool;
	VkSampler sampler;
	VkSampler sampler_nearest;  // slots in kfg_pass.nearest_mask
	VkBuffer ubo;
	VkDeviceMemory ubo_mem;
	uint8_t *ubo_map;
	VkDeviceSize ubo_size, ubo_align, ubo_used;
	kfg_vk_image dummy;
	VkQueryPool queries;
	uint32_t query_count, query_used;
	double timestamp_period_ns;
	bool timestamps;
	char device_name[256];
	uint32_t driver_id;
	char driver_info[520];
} kfg_vk;

// Resolves a plan image reference to a host image. Returns NULL for KFG_ROLE_NONE.
typedef const kfg_vk_image *(*kfg_vk_resolve_fn)(void *user, kfg_image_ref ref);

int kfg_vk_init(kfg_vk *vk, const char *spv_dir, bool validation);
void kfg_vk_destroy(kfg_vk *vk);

int kfg_vk_image_create(kfg_vk *vk, kfg_vk_image *img, VkFormat fmt, uint32_t w, uint32_t h);
void kfg_vk_image_destroy(kfg_vk *vk, kfg_vk_image *img);
VkFormat kfg_vk_format(enum kfg_format f);

// Recording: begin, record plans, submit and wait.
void kfg_vk_begin(kfg_vk *vk);
// Records every pass of the plan; pass i's GPU duration lands in timing slot (first_query + i).
int kfg_vk_record(kfg_vk *vk, const kfg_plan *plan, kfg_vk_resolve_fn resolve, void *user);
int kfg_vk_submit_wait(kfg_vk *vk);
// Durations (ns) of the passes recorded since kfg_vk_begin, in order; returns count.
uint32_t kfg_vk_pass_times(kfg_vk *vk, double *out_ns, uint32_t max);

int kfg_vk_upload_rgba8(kfg_vk *vk, kfg_vk_image *img, const uint8_t *rgba);
// Uploads tightly packed texels of the image's own format (bytes = width * height * texel size).
int kfg_vk_upload_raw(kfg_vk *vk, kfg_vk_image *img, const void *data, size_t bytes);
int kfg_vk_download_rgba8(kfg_vk *vk, const kfg_vk_image *img, uint8_t *rgba);
int kfg_vk_download_rgba16f(kfg_vk *vk, const kfg_vk_image *img, float *rgba);

#endif

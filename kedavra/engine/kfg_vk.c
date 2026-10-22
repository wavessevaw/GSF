// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "kfg_vk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); return -1; } } while (0)

static uint32_t find_memory(kfg_vk *vk, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(vk->phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

static uint32_t *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *buf = malloc((size_t)n + 4);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return buf;
}

VkFormat kfg_vk_format(enum kfg_format f)
{
	switch (f) {
	case KFG_FORMAT_R8: return VK_FORMAT_R8_UNORM;
	case KFG_FORMAT_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
	default: return VK_FORMAT_R16G16B16A16_SFLOAT;
	}
}

static void barrier_to_general(kfg_vk *vk, VkCommandBuffer cb, VkImage image)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	(void)vk;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void full_barrier(VkCommandBuffer cb)
{
	VkMemoryBarrier mb = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static int one_shot_begin(kfg_vk *vk)
{
	VKCHECK(vkResetCommandBuffer(vk->cmd, 0));
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VKCHECK(vkBeginCommandBuffer(vk->cmd, &bi));
	return 0;
}

static int one_shot_end(kfg_vk *vk)
{
	VKCHECK(vkEndCommandBuffer(vk->cmd));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &vk->cmd };
	VKCHECK(vkResetFences(vk->device, 1, &vk->fence));
	VKCHECK(vkQueueSubmit(vk->queue, 1, &si, vk->fence));
	VKCHECK(vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, UINT64_MAX));
	return 0;
}

int kfg_vk_image_create(kfg_vk *vk, kfg_vk_image *img, VkFormat fmt, uint32_t w, uint32_t h)
{
	memset(img, 0, sizeof(*img));
	VkImageCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = fmt,
		.extent = { w, h, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VKCHECK(vkCreateImage(vk->device, &ci, NULL, &img->image));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(vk->device, img->image, &mr);
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
				    .memoryTypeIndex = find_memory(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	if (ai.memoryTypeIndex == UINT32_MAX)
		ai.memoryTypeIndex = find_memory(vk, mr.memoryTypeBits, 0);
	VKCHECK(vkAllocateMemory(vk->device, &ai, NULL, &img->memory));
	VKCHECK(vkBindImageMemory(vk->device, img->image, img->memory, 0));
	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VKCHECK(vkCreateImageView(vk->device, &vi, NULL, &img->view));
	img->format = fmt;
	img->width = w;
	img->height = h;
	if (one_shot_begin(vk))
		return -1;
	barrier_to_general(vk, vk->cmd, img->image);
	VkClearColorValue zero = { .float32 = { 0, 0, 0, 0 } };
	VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage(vk->cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rr);
	return one_shot_end(vk);
}

void kfg_vk_image_destroy(kfg_vk *vk, kfg_vk_image *img)
{
	if (img->view)
		vkDestroyImageView(vk->device, img->view, NULL);
	if (img->image)
		vkDestroyImage(vk->device, img->image, NULL);
	if (img->memory)
		vkFreeMemory(vk->device, img->memory, NULL);
	memset(img, 0, sizeof(*img));
}

int kfg_vk_init(kfg_vk *vk, const char *spv_dir, bool validation)
{
	memset(vk, 0, sizeof(*vk));
	const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "kfg", .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
				     .enabledLayerCount = validation ? 1u : 0u, .ppEnabledLayerNames = layers };
	VKCHECK(vkCreateInstance(&ici, NULL, &vk->instance));

	uint32_t n = 0;
	vkEnumeratePhysicalDevices(vk->instance, &n, NULL);
	if (!n) {
		fprintf(stderr, "no Vulkan device\n");
		return -1;
	}
	VkPhysicalDevice devs[16];
	if (n > 16)
		n = 16;
	vkEnumeratePhysicalDevices(vk->instance, &n, devs);
	const char *want = getenv("KFG_DEVICE");
	uint32_t pick = want ? (uint32_t)atoi(want) : 0;
	if (pick >= n)
		pick = 0;
	vk->phys = devs[pick];
	VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
	VkPhysicalDeviceProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
	vkGetPhysicalDeviceProperties2(vk->phys, &props2);
	snprintf(vk->device_name, sizeof(vk->device_name), "%s", props2.properties.deviceName);
	snprintf(vk->driver_info, sizeof(vk->driver_info), "%s %s", drv.driverName, drv.driverInfo);
	vk->driver_id = drv.driverID;
	vk->timestamp_period_ns = props2.properties.limits.timestampPeriod;
	vk->ubo_align = props2.properties.limits.minUniformBufferOffsetAlignment;
	if (vk->ubo_align < 16)
		vk->ubo_align = 16;

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &qn, NULL);
	VkQueueFamilyProperties qf[16];
	if (qn > 16)
		qn = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &qn, qf);
	vk->queue_family = UINT32_MAX;
	// Prefer a compute-only family, as the compositor does.
	for (uint32_t i = 0; i < qn; i++)
		if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
			vk->queue_family = i;
			break;
		}
	if (vk->queue_family == UINT32_MAX)
		for (uint32_t i = 0; i < qn; i++)
			if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
				vk->queue_family = i;
				break;
			}
	if (vk->queue_family == UINT32_MAX)
		return -1;
	vk->timestamps = qf[vk->queue_family].timestampValidBits > 0;

	VkPhysicalDeviceFeatures feats = { .shaderStorageImageWriteWithoutFormat = VK_TRUE };
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk->queue_family,
					.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
				   .pEnabledFeatures = &feats };
	VKCHECK(vkCreateDevice(vk->phys, &dci, NULL, &vk->device));
	vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);

	VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					.queueFamilyIndex = vk->queue_family };
	VKCHECK(vkCreateCommandPool(vk->device, &pci, NULL, &vk->cmd_pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk->cmd_pool,
					    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VKCHECK(vkAllocateCommandBuffers(vk->device, &cai, &vk->cmd));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VKCHECK(vkCreateFence(vk->device, &fci, NULL, &vk->fence));

	VkDescriptorSetLayoutBinding b[3] = {
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 16, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
	};
	VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b };
	VKCHECK(vkCreateDescriptorSetLayout(vk->device, &dlci, NULL, &vk->dsl));
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &vk->dsl };
	VKCHECK(vkCreatePipelineLayout(vk->device, &plci, NULL, &vk->layout));

	for (int i = 0; i < KFG_SHADER_COUNT; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s.spv", spv_dir, kfg_shader_names[i]);
		size_t size = 0;
		uint32_t *code = read_file(path, &size);
		if (!code) {
			fprintf(stderr, "cannot read %s\n", path);
			return -1;
		}
		VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
		VkShaderModule mod;
		VkResult r = vkCreateShaderModule(vk->device, &smci, NULL, &mod);
		free(code);
		VKCHECK(r);
		VkComputePipelineCreateInfo cpci = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main" },
			.layout = vk->layout,
		};
		r = vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &cpci, NULL, &vk->pipelines[i]);
		vkDestroyShaderModule(vk->device, mod, NULL);
		VKCHECK(r);
	}

	VkDescriptorPoolSize ps[3] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512 * 16 },
	};
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 512, .poolSizeCount = 3, .pPoolSizes = ps };
	VKCHECK(vkCreateDescriptorPool(vk->device, &dpci, NULL, &vk->dpool));

	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	VKCHECK(vkCreateSampler(vk->device, &sci, NULL, &vk->sampler));
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	VKCHECK(vkCreateSampler(vk->device, &sci, NULL, &vk->sampler_nearest));

	vk->ubo_size = 1u << 16;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = vk->ubo_size, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT };
	VKCHECK(vkCreateBuffer(vk->device, &bci, NULL, &vk->ubo));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(vk->device, vk->ubo, &mr);
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
				    .memoryTypeIndex = find_memory(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VKCHECK(vkAllocateMemory(vk->device, &ai, NULL, &vk->ubo_mem));
	VKCHECK(vkBindBufferMemory(vk->device, vk->ubo, vk->ubo_mem, 0));
	VKCHECK(vkMapMemory(vk->device, vk->ubo_mem, 0, vk->ubo_size, 0, (void **)&vk->ubo_map));

	if (vk->timestamps) {
		vk->query_count = 256;
		VkQueryPoolCreateInfo qpci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = vk->query_count };
		VKCHECK(vkCreateQueryPool(vk->device, &qpci, NULL, &vk->queries));
	}
	return kfg_vk_image_create(vk, &vk->dummy, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1);
}

void kfg_vk_destroy(kfg_vk *vk)
{
	if (!vk->device)
		return;
	vkDeviceWaitIdle(vk->device);
	kfg_vk_image_destroy(vk, &vk->dummy);
	if (vk->queries)
		vkDestroyQueryPool(vk->device, vk->queries, NULL);
	vkDestroyBuffer(vk->device, vk->ubo, NULL);
	vkFreeMemory(vk->device, vk->ubo_mem, NULL);
	vkDestroySampler(vk->device, vk->sampler, NULL);
	vkDestroySampler(vk->device, vk->sampler_nearest, NULL);
	vkDestroyDescriptorPool(vk->device, vk->dpool, NULL);
	for (int i = 0; i < KFG_SHADER_COUNT; i++)
		vkDestroyPipeline(vk->device, vk->pipelines[i], NULL);
	vkDestroyPipelineLayout(vk->device, vk->layout, NULL);
	vkDestroyDescriptorSetLayout(vk->device, vk->dsl, NULL);
	vkDestroyFence(vk->device, vk->fence, NULL);
	vkDestroyCommandPool(vk->device, vk->cmd_pool, NULL);
	vkDestroyDevice(vk->device, NULL);
	vkDestroyInstance(vk->instance, NULL);
	memset(vk, 0, sizeof(*vk));
}

void kfg_vk_begin(kfg_vk *vk)
{
	vkResetDescriptorPool(vk->device, vk->dpool, 0);
	vk->ubo_used = 0;
	vk->query_used = 0;
	vkResetCommandBuffer(vk->cmd, 0);
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	vkBeginCommandBuffer(vk->cmd, &bi);
	if (vk->timestamps)
		vkCmdResetQueryPool(vk->cmd, vk->queries, 0, vk->query_count);
}

int kfg_vk_record(kfg_vk *vk, const kfg_plan *plan, kfg_vk_resolve_fn resolve, void *user)
{
	for (uint32_t i = 0; i < plan->n; i++) {
		const kfg_pass *x = &plan->pass[i];
		const kfg_vk_image *dst = resolve(user, x->dst);
		if (!dst) {
			fprintf(stderr, "pass %u (%s): no destination\n", i, x->label);
			return -1;
		}
		VkDeviceSize off = vk->ubo_used;
		if (off + sizeof(kfg_params) > vk->ubo_size)
			return -1;
		memcpy(vk->ubo_map + off, &x->params, sizeof(kfg_params));
		vk->ubo_used = (off + sizeof(kfg_params) + vk->ubo_align - 1) / vk->ubo_align * vk->ubo_align;

		VkDescriptorSet set;
		VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = vk->dpool,
						    .descriptorSetCount = 1, .pSetLayouts = &vk->dsl };
		VKCHECK(vkAllocateDescriptorSets(vk->device, &dai, &set));
		VkDescriptorBufferInfo dbi = { vk->ubo, off, sizeof(kfg_params) };
		VkDescriptorImageInfo dii = { .imageView = dst->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo sii[16];
		for (uint32_t s = 0; s < 16; s++) {
			const kfg_vk_image *img = s < x->nslots ? resolve(user, x->slots[s]) : NULL;
			if (!img)
				img = &vk->dummy;
			sii[s] = (VkDescriptorImageInfo){ .sampler = (x->nearest_mask >> s) & 1u ? vk->sampler_nearest : vk->sampler, .imageView = img->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
		}
		VkWriteDescriptorSet w[3] = {
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dii },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 16,
			  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = sii },
		};
		vkUpdateDescriptorSets(vk->device, 3, w, 0, NULL);
		vkCmdBindPipeline(vk->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->pipelines[x->shader]);
		vkCmdBindDescriptorSets(vk->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->layout, 0, 1, &set, 0, NULL);
		if (vk->timestamps && vk->query_used + 2 <= vk->query_count)
			vkCmdWriteTimestamp(vk->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk->queries, vk->query_used);
		vkCmdDispatch(vk->cmd, x->groups[0], x->groups[1], 1);
		if (vk->timestamps && vk->query_used + 2 <= vk->query_count) {
			vkCmdWriteTimestamp(vk->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk->queries, vk->query_used + 1);
			vk->query_used += 2;
		}
		full_barrier(vk->cmd);
	}
	return 0;
}

int kfg_vk_submit_wait(kfg_vk *vk)
{
	return one_shot_end(vk);
}

uint32_t kfg_vk_pass_times(kfg_vk *vk, double *out_ns, uint32_t max)
{
	if (!vk->timestamps || !vk->query_used)
		return 0;
	uint64_t ts[256];
	if (vkGetQueryPoolResults(vk->device, vk->queries, 0, vk->query_used, sizeof(ts), ts, sizeof(uint64_t),
				  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
		return 0;
	uint32_t n = vk->query_used / 2;
	if (n > max)
		n = max;
	for (uint32_t i = 0; i < n; i++)
		out_ns[i] = (double)(ts[2 * i + 1] - ts[2 * i]) * vk->timestamp_period_ns;
	return n;
}

static int staging(kfg_vk *vk, VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, void **map)
{
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
				   .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VKCHECK(vkCreateBuffer(vk->device, &bci, NULL, buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(vk->device, *buf, &mr);
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
				    .memoryTypeIndex = find_memory(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VKCHECK(vkAllocateMemory(vk->device, &ai, NULL, mem));
	VKCHECK(vkBindBufferMemory(vk->device, *buf, *mem, 0));
	VKCHECK(vkMapMemory(vk->device, *mem, 0, size, 0, map));
	return 0;
}

static int copy(kfg_vk *vk, const kfg_vk_image *img, void *data, VkDeviceSize bytes, bool to_image)
{
	VkBuffer buf;
	VkDeviceMemory mem;
	void *map;
	if (staging(vk, bytes, &buf, &mem, &map))
		return -1;
	if (to_image)
		memcpy(map, data, bytes);
	if (one_shot_begin(vk))
		return -1;
	full_barrier(vk->cmd);
	VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { img->width, img->height, 1 } };
	if (to_image)
		vkCmdCopyBufferToImage(vk->cmd, buf, img->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
	else
		vkCmdCopyImageToBuffer(vk->cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
	full_barrier(vk->cmd);
	int rc = one_shot_end(vk);
	if (!to_image && !rc)
		memcpy(data, map, bytes);
	vkUnmapMemory(vk->device, mem);
	vkDestroyBuffer(vk->device, buf, NULL);
	vkFreeMemory(vk->device, mem, NULL);
	return rc;
}

int kfg_vk_upload_rgba8(kfg_vk *vk, kfg_vk_image *img, const uint8_t *rgba)
{
	return copy(vk, img, (void *)rgba, (VkDeviceSize)img->width * img->height * 4, true);
}

int kfg_vk_upload_raw(kfg_vk *vk, kfg_vk_image *img, const void *data, size_t bytes)
{
	return copy(vk, img, (void *)data, (VkDeviceSize)bytes, true);
}

int kfg_vk_download_rgba8(kfg_vk *vk, const kfg_vk_image *img, uint8_t *rgba)
{
	return copy(vk, img, rgba, (VkDeviceSize)img->width * img->height * 4, false);
}

static float half_to_float(uint16_t h)
{
	uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
	if (e == 0)
		f = m ? 0 : s; // flush denormals
	else if (e == 31)
		f = s | 0x7f800000u | (m << 13);
	else
		f = s | ((e + 112) << 23) | (m << 13);
	if (e == 0 && m)
		return (s ? -1.f : 1.f) * ldexpf((float)m, -24);
	float out;
	memcpy(&out, &f, 4);
	return out;
}

int kfg_vk_download_rgba16f(kfg_vk *vk, const kfg_vk_image *img, float *rgba)
{
	size_t n = (size_t)img->width * img->height * 4;
	uint16_t *tmp = malloc(n * 2);
	if (!tmp)
		return -1;
	int rc = copy(vk, img, tmp, n * 2, false);
	for (size_t i = 0; i < n; i++)
		rgba[i] = half_to_float(tmp[i]);
	free(tmp);
	return rc;
}

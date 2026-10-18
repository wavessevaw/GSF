/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GFG_OUTPUT_WORKER_H
#define GFG_OUTPUT_WORKER_H
#include <pthread.h>
#include "gpu_bridge.h"

typedef struct gfg_output gfg_output;
/* One executing job; real FIFO bounded by swapchain images. No caller pointers survive submit. */
gfg_output *gfg_output_create(gfg_gpu_fg *gpu, VkQueue queue,
    PFN_vkQueuePresentKHR present, const gfg_present_id_hooks *ids, pthread_mutex_t *wsi,
    VkDevice device, PFN_vkGetDeviceProcAddr gdpa, uint32_t image_count, const VkSemaphore *acquired);
VkResult gfg_output_submit(gfg_output *out, const VkPresentInfoKHR *app, uint64_t source_ns, int *handled);
/* id=0 drains all jobs; nonzero waits only until that application ID was handed off. */
VkResult gfg_output_drain(gfg_output *out, uint64_t id, uint64_t timeout, uint64_t *remaining);
pthread_mutex_t *gfg_output_wsi(gfg_output *out);
int gfg_output_waiting_image(gfg_output *out);
VkResult gfg_output_status(gfg_output *out);
int gfg_output_sem_reusable(gfg_output *out, VkSemaphore semaphore);
void gfg_output_acquired(gfg_output *out, uint32_t image, VkSemaphore semaphore);
void gfg_output_stop(gfg_output *out); /* join before draining/destroying the GPU bridge */
/* Call after the bridge GPU drain; private per-image gates must no longer be in use. */
void gfg_output_destroy(gfg_output *out);
#endif

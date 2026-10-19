/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GSF_BUFFERED_OUTPUT_H
#define GSF_BUFFERED_OUTPUT_H
#include "gpu_bridge.h"
#include "output_proxy_bootstrap.h"
typedef struct gsf_buffered_output gsf_buffered_output;
/* Conservative barrier-based, single-queue Vulkan contract. Unsupported
 * create metadata is rejected BEFORE returning a virtual handle. */
gsf_buffered_output *gsf_buffered_create(const gfg_gpu_fg_create_info *info,
    const VkSwapchainCreateInfoKHR *source,const VkSwapchainCreateInfoKHR *real,
    PFN_vkCreateSwapchainKHR create,PFN_vkDestroySwapchainKHR destroy,
    PFN_vkQueuePresentKHR present,const gfg_offscreen_dispatch *memory);
gfg_output_proxy *gsf_buffered_proxy(gsf_buffered_output *owner);
int gsf_buffered_uses_queue(const gsf_buffered_output *owner,VkQueue queue);
VkResult gsf_buffered_status(gsf_buffered_output *owner);
VkResult gsf_buffered_drain(gsf_buffered_output *owner);
void gsf_buffered_stop(gsf_buffered_output *owner);
void gsf_buffered_destroy(gsf_buffered_output *owner);
#endif

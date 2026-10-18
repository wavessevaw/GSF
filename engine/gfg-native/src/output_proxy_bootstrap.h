/* SPDX-License-Identifier: GPL-3.0-or-later
 * EXPERIMENTAL Vulkan virtual swapchain bootstrap.
 *
 * This owns the real compositor output swapchain and a *different* array of
 * app-facing VkImages. Neither the app-visible pseudo handle nor its source
 * images may be passed to the downstream WSI present/destroy entrypoints.
 *
 * Not a complete game interception: a real layer still requires GPU-backed
 * acquire signalling, async render-wait/copy completion, present scheduling,
 * swapchain extension/lifetime handling and native compositor verification.
 */
#ifndef GFG_OUTPUT_PROXY_BOOTSTRAP_H
#define GFG_OUTPUT_PROXY_BOOTSTRAP_H
#include "offscreen_source_pool.h"
#include "virtual_wsi_facade.h"

typedef struct {
    gfg_offscreen_source_pool source;
    gfg_virtual_wsi app;
    VkSwapchainKHR output;
    VkDevice device;
    VkPhysicalDevice physical;
    int app_initialized;
} gfg_output_proxy;

static inline void gfg_output_proxy_destroy(gfg_output_proxy *p,
        PFN_vkDestroySwapchainKHR destroy_output,
        const gfg_offscreen_dispatch *dispatch) {
    if (!p) return;
    /* Caller has first stopped every rendering/copy/present worker, waited for
     * GPU retirement, and joined concurrent app API calls. This function is
     * deliberately NOT a substitute for wait-idle/lifetime management. */
    if (p->app_initialized) {
        gfg_vwsi_close(&p->app);
        gfg_vwsi_fini(&p->app);
        p->app_initialized = 0;
    }
    gfg_offscreen_pool_destroy(&p->source, p->device, dispatch);
    if (p->output && destroy_output)
        destroy_output(p->device, p->output, NULL);
    p->output = VK_NULL_HANDLE;
}

/* The unique virtual app handle is the address of the persistent owner, not
 * an image, and it never escapes to real WSI. The caller must allocate 'p'
 * in stable memory (not relocate it) until all app references are retired.
 * Failure leaves no output swapchain or source image alive. */
static inline VkResult gfg_output_proxy_create_ex(gfg_output_proxy *p,
        VkDevice device, VkPhysicalDevice physical,
        const VkSwapchainCreateInfoKHR *original,
        const VkSwapchainCreateInfoKHR *downstream_info,
        PFN_vkCreateSwapchainKHR create_output,
        PFN_vkDestroySwapchainKHR destroy_output,
        const gfg_offscreen_dispatch *dispatch,
        void *callback_user,
        gfg_vwsi_acquire_signal acquire_signal,
        gfg_vwsi_copy_submit submit_capture) {
    if (!p || !device || !physical || !original || !downstream_info ||
        !create_output ||
        !destroy_output || !gfg_offscreen_dispatch_complete(dispatch) ||
        !acquire_signal || !submit_capture)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (p->output || p->app_initialized || p->source.count)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (gfg_offscreen_unsupported(original) ||
        (original->imageUsage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT))
        return VK_ERROR_FEATURE_NOT_PRESENT;

    /* Only the real WSI is created downstream. It is never separately
     * presented by the application. Transfer DST is mandatory for output
     * ownership; if unsupported, fall back to the unmodified game path. */
    /* Do not mutate the driver's Gamescope maintenance1 mode chain.
     * The virtual app source and real compositor-facing WSI can have
     * distinct present modes because they are separate Vulkan resources. */
    VkSwapchainCreateInfoKHR real = *downstream_info;
    real.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    p->device = device;
    p->physical = physical;
    VkResult result = create_output(device, &real, NULL, &p->output);
    if (result != VK_SUCCESS) {
        p->output = VK_NULL_HANDLE;
        return result;
    }
    result = gfg_offscreen_pool_create(&p->source,device,physical,original,dispatch);
    if (result != VK_SUCCESS) goto rollback;
    const VkSwapchainKHR app_handle =
        (VkSwapchainKHR)(uintptr_t)(const void *)p;
    result = gfg_vwsi_init(&p->app,app_handle,p->source.images,p->source.count,
            original->imageFormat,original->imageExtent,callback_user,
            acquire_signal,submit_capture);
    if (result != VK_SUCCESS) goto rollback;
    p->app_initialized = 1;
    return VK_SUCCESS;
rollback:
    gfg_output_proxy_destroy(p,destroy_output,dispatch);
    return result;
}

/* Old entrypoint for all pre-existing Desktop/create-only test users.
 * No runtime WSI contract is changed without an explicit downstream_info. */
static inline VkResult gfg_output_proxy_create(gfg_output_proxy *p,
        VkDevice device, VkPhysicalDevice physical,
        const VkSwapchainCreateInfoKHR *original,
        PFN_vkCreateSwapchainKHR create_output,
        PFN_vkDestroySwapchainKHR destroy_output,
        const gfg_offscreen_dispatch *dispatch,
        void *callback_user,
        gfg_vwsi_acquire_signal acquire_signal,
        gfg_vwsi_copy_submit submit_capture) {
    return gfg_output_proxy_create_ex(p,device,physical,original,original,
        create_output,destroy_output,dispatch,callback_user,
        acquire_signal,submit_capture);
}

static inline VkSwapchainKHR gfg_output_proxy_app_handle(
        const gfg_output_proxy *p) {
    return p && p->app_initialized ? p->app.handle : VK_NULL_HANDLE;
}
#endif

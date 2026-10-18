/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synchronous, caller-immutable handling of single-swapchain present metadata.
 * Completion fences and display times belong to the REAL present only.
 * Incremental damage is omitted because FG writes the whole output image.
 * The bridge's create-time FIFO contract also controls dynamic present modes.
 * Unknown metadata fails closed; never truncate an unknown structure.
 */
#ifndef GFG_PRESENT_CHAIN_H
#define GFG_PRESENT_CHAIN_H
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define GFG_PRESENT_CHAIN_MAX 8u
typedef union {
    VkBaseOutStructure base;
    VkPresentIdKHR id1;
    VkPresentId2KHR id2;
    VkSwapchainPresentFenceInfoEXT fence;
    VkSwapchainPresentModeInfoEXT mode;
    VkPresentTimesInfoGOOGLE times;
    VkPresentTimingsInfoEXT timings;
    VkDeviceGroupPresentInfoKHR group;
    VkPresentRegionsKHR regions;
} gfg_present_node;

typedef struct {
    gfg_present_node nodes[GFG_PRESENT_CHAIN_MAX];
    uint32_t count;
    int id_index;
    uint64_t app_id, layer_id;
    VkPresentModeKHR requested_mode, effective_mode;
    int dynamic_mode;
    VkStructureType rejected_type;
} gfg_present_chain;

/* Borrowed arrays remain valid for the enclosing synchronous present call.
 * ID mode 0 preserves the app's namespace (synthetics omit IDs); a nonzero
 * mode is the optional layer-owned tracing namespace. Only our copied ID and
 * mode nodes can point to rewritten private array values. */
static inline const char *gfg_pc_prepare(gfg_present_chain *out,
                                        const VkPresentInfoKHR *app, int id_mode) {
    memset(out, 0, sizeof(*out));
    out->id_index = -1;
    out->effective_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (app->swapchainCount != 1) return "multiple-swapchains";
    uint32_t seen = 0, walked = 0;
    for (const VkBaseInStructure *n = app->pNext; n; n = n->pNext) {
        out->rejected_type = n->sType;
        if (++walked > GFG_PRESENT_CHAIN_MAX) return "present-chain-too-long";
        uint32_t bit;
        size_t size;
        switch (n->sType) {
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
            const VkPresentIdKHR *p = (const VkPresentIdKHR *)n;
            if (id_mode && id_mode != 1) return "present-id-mode-mismatch";
            if (p->swapchainCount != 1) return "invalid-present-id";
            out->app_id = p->pPresentIds ? p->pPresentIds[0] : 0;
            out->id_index = (int)out->count;
            bit = 1u; size = sizeof(*p); break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR: {
            const VkPresentId2KHR *p = (const VkPresentId2KHR *)n;
            if (id_mode && id_mode != 2) return "present-id-mode-mismatch";
            if (p->swapchainCount != 1) return "invalid-present-id";
            out->app_id = p->pPresentIds ? p->pPresentIds[0] : 0;
            out->id_index = (int)out->count;
            bit = 1u; size = sizeof(*p); break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT: {
            const VkSwapchainPresentFenceInfoEXT *p = (const VkSwapchainPresentFenceInfoEXT *)n;
            if (p->swapchainCount != 1 || !p->pFences) return "invalid-present-fence";
            bit = 2u; size = sizeof(*p); break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT: {
            const VkSwapchainPresentModeInfoEXT *p = (const VkSwapchainPresentModeInfoEXT *)n;
            if (p->swapchainCount != 1 || !p->pPresentModes) return "invalid-present-mode";
            out->requested_mode = p->pPresentModes[0];
            out->dynamic_mode = 1;
            bit = 4u; size = sizeof(*p); break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR: {
            const VkPresentRegionsKHR *p = (const VkPresentRegionsKHR *)n;
            if (p->swapchainCount != 1) return "invalid-present-regions";
            if (seen & 8u) return "duplicate-present-metadata";
            seen |= 8u;
            /* Omission means full-surface damage, including after a synthetic. */
            continue;
        }
        case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE: {
            const VkPresentTimesInfoGOOGLE *p = (const VkPresentTimesInfoGOOGLE *)n;
            if (p->swapchainCount != 1) return "invalid-present-times";
            bit = 16u; size = sizeof(*p); break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT: {
            const VkPresentTimingsInfoEXT *p = (const VkPresentTimingsInfoEXT *)n;
            /* App-owned IDs stay unchanged so its timing-query results stay in
             * its namespace. Relative deadlines need a separate cadence policy. */
            if (id_mode) return "present-timing-needs-application-owned-ids";
            if (p->swapchainCount != 1) return "invalid-present-timings";
            /* Vulkan permits a NULL array: there are no per-present timing
             * records to inspect. Preserve that no-op node and its suffix. */
            if (p->pTimingInfos &&
                (p->pTimingInfos[0].flags & VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT))
                return "relative-present-timing";
            bit = 32u; size = sizeof(*p); break;
        }
        default:
            return "unsupported-present-metadata";
        }
        if (seen & bit) return "duplicate-present-metadata";
        seen |= bit;
        gfg_present_node *copy = &out->nodes[out->count];
        memcpy(copy, n, size);
        copy->base.pNext = NULL;
        if (out->count) out->nodes[out->count - 1].base.pNext = &copy->base;
        ++out->count;
        if (n->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT)
            copy->mode.pPresentModes = &out->effective_mode;
    }
    out->rejected_type = 0;
    return NULL;
}

static inline const void *gfg_pc_head(const gfg_present_chain *chain) {
    return chain->count ? &chain->nodes[0] : NULL;
}

/* Fallback can retain metadata the generator does not understand. If a
 * create-time FIFO override changed the available mode list, copy only the
 * known prefix through its mode node and preserve the entire suffix verbatim.
 * An unknown prefix is never truncated or silently reinterpreted. */
static inline int gfg_pc_fifo_fallback(gfg_present_chain *out, const VkPresentInfoKHR *app,
                                      VkPresentInfoKHR *real) {
    const VkBaseInStructure *n=app->pNext;uint32_t target=GFG_PRESENT_CHAIN_MAX,at=0;
    for(;n && at<GFG_PRESENT_CHAIN_MAX;n=n->pNext,++at)
        if(n->sType==VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT){target=at;break;}
    if(n && at==GFG_PRESENT_CHAIN_MAX)return 0;
    if(target==GFG_PRESENT_CHAIN_MAX)return 1;
    memset(out,0,sizeof(*out));out->effective_mode=VK_PRESENT_MODE_FIFO_KHR;
    n=app->pNext;
    for(at=0;at<=target;++at,n=n->pNext){
        size_t size;
        switch(n->sType){
#define NODE(type,structure) case type:size=sizeof(structure);break
        NODE(VK_STRUCTURE_TYPE_PRESENT_ID_KHR,VkPresentIdKHR);
        NODE(VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,VkPresentId2KHR);
        NODE(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,VkSwapchainPresentFenceInfoEXT);
        NODE(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,VkSwapchainPresentModeInfoEXT);
        NODE(VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,VkPresentTimesInfoGOOGLE);
        NODE(VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,VkPresentTimingsInfoEXT);
        NODE(VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,VkDeviceGroupPresentInfoKHR);
        NODE(VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,VkPresentRegionsKHR);
#undef NODE
        default:return 0;
        }
        memcpy(&out->nodes[at],n,size);
        if(at)out->nodes[at-1].base.pNext=&out->nodes[at].base;
    }
    out->nodes[target].mode.pPresentModes=&out->effective_mode;
    real->pNext=&out->nodes[0];return 1;
}

/* Assign every real present its private ID even when the app's ID was at the
 * tail of a fence/mode chain. The application node/array is never modified. */
static inline void gfg_pc_set_id(gfg_present_chain *chain, int mode, uint64_t id) {
    if (!mode || !id) return;
    chain->layer_id = id;
    if (chain->id_index < 0) {
        const uint32_t at = chain->count++;
        chain->id_index = (int)at;
        if (at) chain->nodes[at - 1].base.pNext = &chain->nodes[at].base;
        if (mode == 2) chain->nodes[at].id2 = (VkPresentId2KHR) {
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, .swapchainCount = 1};
        else chain->nodes[at].id1 = (VkPresentIdKHR) {
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1};
    }
    if (mode == 2) chain->nodes[chain->id_index].id2.pPresentIds = &chain->layer_id;
    else chain->nodes[chain->id_index].id1.pPresentIds = &chain->layer_id;
}
#endif

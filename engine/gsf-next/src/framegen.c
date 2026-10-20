/* Frame generation: FidelityFX Optical Flow 1.1.2 (seven compute passes, host sequencing ported
 * from the SDK's ffx_opticalflow.cpp, see shaders/fidelityfx/NOTICE.md) plus the layer's own
 * interpolation shader (shaders/afmf_interpolate.comp). This file creates and destroys one
 * swapchain's generation; the pieces live in the framegen_*.c files next to it. */

#include "framegen_internal.h"
#include <stdatomic.h>

static atomic_uint_fast64_t g_context_id;

void afmf_framegen_set_family(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t family)
{
    if (fg->secondary_pool != VK_NULL_HANDLE || dev->set_loader_data == NULL)
        return;
    uint32_t count = fg->slots * 4u;
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    if (dev->fns.create_command_pool(dev->handle, &pool, NULL, &fg->secondary_pool) != VK_SUCCESS)
        return;
    VkCommandBuffer *cmds = calloc(count, sizeof *cmds);
    fg->secondaries = calloc(count, sizeof *fg->secondaries);
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = fg->secondary_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
        .commandBufferCount = count,
    };
    bool ok = cmds != NULL && fg->secondaries != NULL &&
              dev->fns.allocate_command_buffers(dev->handle, &alloc, cmds) == VK_SUCCESS;
    /* Allocated below the loader's trampoline: stamp the dispatch pointer ourselves. */
    for (uint32_t i = 0; ok && i < count; i++)
        ok = dev->set_loader_data(dev->handle, cmds[i]) == VK_SUCCESS;
    if (ok) {
        for (uint32_t i = 0; i < count; i++)
            fg->secondaries[i].cmd = cmds[i];
    } else {
        AFMF_WARN("pre-recorded command buffers unavailable; recording every frame");
        dev->fns.destroy_command_pool(dev->handle, fg->secondary_pool, NULL);
        fg->secondary_pool = VK_NULL_HANDLE;
        free(fg->secondaries);
        fg->secondaries = NULL;
    }
    free(cmds);
}

uint32_t afmf_framegen_owner_slots(const struct afmf_framegen *fg) { return fg ? fg->slots : 0u; }
uint32_t afmf_framegen_owner_sources(const struct afmf_framegen *fg) { return fg ? fg->owner_registered_sources : 0u; }
uint32_t afmf_framegen_owner_targets(const struct afmf_framegen *fg) { return fg ? fg->target_count : 0u; }
uint32_t afmf_framegen_owner_output_stride(const struct afmf_framegen *fg) {
    return fg ? fg->batch_stride : 0u;
}

void afmf_framegen_destroy(struct afmf_device *dev, struct afmf_framegen *fg)
{
    if (fg == NULL)
        return;
    if (fg->secondary_pool != VK_NULL_HANDLE)
        dev->fns.destroy_command_pool(dev->handle, fg->secondary_pool, NULL); /* frees the buffers */
    free(fg->secondaries);
    for (uint32_t j = 0; fg->target_views != NULL && j < fg->target_count; j++)
        if (fg->target_views[j] != VK_NULL_HANDLE)
            dev->fns.destroy_image_view(dev->handle, fg->target_views[j], NULL);
    free(fg->target_views);
    for (uint32_t j = 0; fg->source_views != NULL && j < fg->source_count; j++)
        if (fg->source_views[j] != VK_NULL_HANDLE)
            dev->fns.destroy_image_view(dev->handle, fg->source_views[j], NULL);
    free(fg->source_views);
    free(fg->direct_sets);
    free(fg->batch_sets);
    free(fg->ingest_sets);
    if (fg->pool != VK_NULL_HANDLE)
        dev->fns.destroy_descriptor_pool(dev->handle, fg->pool, NULL); /* frees the sets */
    if (fg->queries != VK_NULL_HANDLE) {
        /* The caller has waited for every slot; fold the last results in and report. */
        for (uint32_t s = 0; s < fg->slots; s++)
            afmf_framegen_profiler_collect(dev, fg, s);
        afmf_framegen_profiler_report(fg);
        dev->fns.destroy_query_pool(dev->handle, fg->queries, NULL);
    }
    free(fg->query_renderer);
    free(fg->query_epoch);
    free(fg->query_tunable);
    free(fg->query_pair);
    free(fg->query_outputs);
    free(fg->query_phases);
    free(fg->query_count);
    free(fg->query_stage);
    if (fg->dump_mapped != NULL)
        dev->fns.unmap_memory(dev->handle, fg->dump_memory);
    if (fg->dump_buffer != VK_NULL_HANDLE)
        dev->fns.destroy_buffer(dev->handle, fg->dump_buffer, NULL);
    if (fg->dump_memory != VK_NULL_HANDLE)
        dev->fns.free_memory(dev->handle, fg->dump_memory, NULL);
    if (fg->cb_mapped != NULL)
        dev->fns.unmap_memory(dev->handle, fg->cb_memory);
    if (fg->cb != VK_NULL_HANDLE)
        dev->fns.destroy_buffer(dev->handle, fg->cb, NULL);
    if (fg->cb_memory != VK_NULL_HANDLE)
        dev->fns.free_memory(dev->handle, fg->cb_memory, NULL);
    if (fg->sampler != VK_NULL_HANDLE)
        dev->fns.destroy_sampler(dev->handle, fg->sampler, NULL);
    afmf_framegen_image_destroy(dev, &fg->output);
    afmf_framegen_image_destroy(dev, &fg->color_half);
    for (uint32_t p = 0; p < 2; p++) {
        afmf_framegen_image_destroy(dev, &fg->color[p]);
        for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
            afmf_framegen_image_destroy(dev, &fg->luma[p][k]);
            afmf_framegen_image_destroy(dev, &fg->flow[p][k]);
        }
    }
    afmf_framegen_image_destroy(dev, &fg->flow_out);
    afmf_framegen_image_destroy(dev, &fg->scd_histogram);
    afmf_framegen_image_destroy(dev, &fg->scd_previous_histogram);
    afmf_framegen_image_destroy(dev, &fg->scd_temp);
    afmf_framegen_image_destroy(dev, &fg->scd_output);
    free(fg);
}

/* Why generation cannot run on this device and swapchain, or NULL. */
static const char *blocker_for(struct afmf_device *dev, struct afmf_framegen *fg,
                               VkFormat swapchain_format, VkExtent2D extent, bool half)
{
    /* What the flow shaders use: subgroupElect and the ids (basic), subgroupAdd and
     * subgroupMin (arithmetic), the pyramid's quad swaps (quad); all in compute. */
    const VkSubgroupFeatureFlags subgroup_ops = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                                VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                                VK_SUBGROUP_FEATURE_QUAD_BIT;
    if (dev->api_version < VK_API_VERSION_1_1)
        return "application uses Vulkan 1.0; the flow shaders need 1.1 subgroups";
    if ((dev->subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0 ||
        (dev->subgroup.supportedOperations & subgroup_ops) != subgroup_ops)
        return "device lacks subgroup basic, arithmetic and quad operations in compute";
    if (!afmf_framegen_describe_format(swapchain_format, fg))
        return "swapchain format has no interpolation variant";
    if (extent.width < AFMF_MIN_EXTENT || extent.height < AFMF_MIN_EXTENT)
        return "swapchain too small for the optical flow pyramid";
    if (!afmf_framegen_format_supports(dev, fg->out_format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !afmf_framegen_format_supports(dev, fg->color_format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        return "device lacks storage or filtered sampling for the swapchain format";
    if (half && !fg->ingest_direct &&
        !afmf_framegen_format_supports(dev, downsample_variants[fg->half].format,
                                       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        return "device lacks storage for the half-resolution colour";
    if (!afmf_framegen_format_supports(dev, VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT))
        return "device lacks r32ui image atomics";
    if (!afmf_framegen_format_supports(dev, VK_FORMAT_R8_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !afmf_framegen_format_supports(dev, VK_FORMAT_R16G16_SINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return "device lacks r8ui or rg16i storage images (shaderStorageImageExtendedFormats)";
    if (afmf_framegen_pipelines_get(dev) == NULL)
        return "pipelines unavailable";
    return NULL;
}

struct afmf_framegen *afmf_framegen_create(struct afmf_device *dev, VkFormat swapchain_format,
                                           VkExtent2D extent, uint32_t slots,
                                           const VkImage *images, uint32_t image_count,
                                           const VkImage *targets, uint32_t target_count, bool direct_ingest, const struct afmf_config *cfg)
{
    struct afmf_framegen *fg = calloc(1, sizeof *fg);
    if (fg == NULL)
        return NULL;
    fg->context_id = atomic_fetch_add_explicit(&g_context_id,1,memory_order_relaxed)+1;
    fg->diagnostics=cfg->diagnostics;
    const char *detail=getenv("GSF_NEXT_PROFILE_DETAIL");
    fg->detailed=cfg->diagnostics && !(detail!=NULL && strcmp(detail,"0")==0);
    fg->requested=gsf_renderer_requested();
    /* Only the independent private owner may request 2-lane batching.
     * The existing game-facing swapchain remains on legacy 3-lane indices.
     * Default owner x3 is one batched reconstruction per pair, not two
     * full-resolution interpolation dispatches. */
    const bool owner_x2=cfg->owner_target_stride==1u &&
                          slots>0u && target_count==slots;
    const bool owner_x3=cfg->owner_target_stride==2u &&
                          slots>0u && target_count==slots*2u;
    fg->batch_stride=owner_x3 ? 2u : 3u;
    if(fg->requested==GSF_RENDER_AUTO) {
        /* Native Output Owner owns unaliased storage output textures.
         * Direct x2 eliminates a full-frame scratch write+copy; batched x3
         * reuses one full-resolution pass for the two temporal phases.
         * If storage format is unavailable, existing copy fallback remains. */
        if(owner_x2)fg->requested=GSF_RENDER_DIRECT;
        else if(owner_x3)fg->requested=GSF_RENDER_BATCH;
    }
    fg->last_renderer=GSF_RENDER_AUTO;
    fg->extent = extent;
    fg->slots = slots;
    fg->swapchain_format = swapchain_format;
    fg->dump_from = AFMF_DUMP_FIRST;
    bool have_images = images != NULL && image_count > 0 && image_count <= AFMF_MAX_IMAGES;
    fg->direct_variant = afmf_framegen_describe_format(swapchain_format,fg) ? fg->variant : VARIANT_COUNT;
    fg->direct = targets != NULL && target_count > 0 && target_count <= AFMF_MAX_IMAGES &&
                 fg->direct_variant != VARIANT_COUNT &&
                 afmf_framegen_format_supports(dev,fg->out_format,VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    fg->ingest_variant = afmf_framegen_ingest_variant_for(dev, swapchain_format);
    fg->ingest_direct = have_images && direct_ingest && fg->ingest_variant != VARIANT_COUNT;
    fg->automatic=fg->direct && fg->requested==GSF_RENDER_AUTO && cfg->profile;
    fg->batch=fg->direct && target_count%fg->batch_stride==0u &&
        (fg->automatic || fg->requested==GSF_RENDER_BATCH);
    fg->renderer=fg->requested==GSF_RENDER_BATCH && fg->batch ? GSF_RENDER_BATCH :
        fg->requested==GSF_RENDER_DIRECT && fg->direct ? GSF_RENDER_DIRECT : GSF_RENDER_COPY;
    fg->owner_registered_sources = have_images ? image_count : 0u;
    fg->source_count = fg->ingest_direct ? image_count : 0;
    fg->target_count = fg->direct ? target_count : 0;

    /* Performance mode: the block search, 85 % of the cost, runs on a half-size frame; blocks
     * become 16 pixels on screen. Auto picks it from 1440p up, where the search dominates. */
    const bool economy=cfg->performance==AFMF_PERFORMANCE_ECONOMY;
    bool half = economy || cfg->performance==AFMF_PERFORMANCE_FAST ||
                (cfg->performance==AFMF_PERFORMANCE_AUTO &&
                 (uint64_t)extent.width*extent.height>=2560u*1440u);
    if(extent.width/2u<AFMF_MIN_EXTENT || extent.height/2u<AFMF_MIN_EXTENT)
        half=false;
    /* Budget-first: 1280x800 -> 320x200 optical flow (vs 640x400).
     * Full-resolution colour history and interpolation are preserved. */
    fg->flow_scale=economy && extent.width/4u>=AFMF_MIN_EXTENT &&
                   extent.height/4u>=AFMF_MIN_EXTENT ? 4u : half?2u:1u;
    fg->of_extent.width = (extent.width + fg->flow_scale - 1) / fg->flow_scale;
    fg->of_extent.height = (extent.height + fg->flow_scale - 1) / fg->flow_scale;
    fg->half = swapchain_format == VK_FORMAT_R16G16B16A16_SFLOAT ? HALF_RGBA16F : HALF_RGBA8;
    /* At reduced flow resolution five levels already cover +-128 flow pixels (+-256 on screen): the
     * two coarsest levels are drains, not accuracy. `high` still forces all seven. */
    fg->levels=economy?3u:
        cfg->search_mode==AFMF_SEARCH_STANDARD?5u:7u;
    if(!economy && fg->flow_scale>1u &&
       cfg->search_mode!=AFMF_SEARCH_HIGH && fg->levels>5u)
        fg->levels=5u;
    if (fg->levels > AFMF_LEVELS)
        fg->levels = AFMF_LEVELS;
    fg->max_levels = fg->levels;

    const char *blocker = blocker_for(dev, fg, swapchain_format, extent, half);
    VkResult res = VK_SUCCESS;
    if (blocker == NULL && fg->batch && dev->framegen_pipelines->batch[fg->direct_variant] == VK_NULL_HANDLE)
        fg->batch = false;
    if (blocker == NULL) {
        /* Game sources and private outputs have different cardinalities and lifetimes.
         * Never alias their views or use a source index to address an output descriptor. */
        const VkImage *groups[2] = {images, targets};
        uint32_t counts[2] = {fg->source_count, fg->target_count};
        VkImageView **views[2] = {&fg->source_views, &fg->target_views};
        for (uint32_t g = 0; g < 2 && res == VK_SUCCESS; g++) {
            if (counts[g] == 0) continue;
            *views[g] = calloc(counts[g], sizeof **views[g]);
            if (*views[g] == NULL) { res = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
            for (uint32_t j = 0; j < counts[g] && res == VK_SUCCESS; j++) {
                VkImageViewCreateInfo view = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = groups[g][j], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = g == 0 ? swapchain_format : fg->out_format,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                };
                res = dev->fns.create_image_view(dev->handle, &view, NULL, &(*views[g])[j]);
            }
        }
        if (res != VK_SUCCESS) blocker = "source or private-output views failed";
    }
    if (blocker == NULL) {
        res = afmf_framegen_resources_create(dev, fg);
        if (res == VK_SUCCESS)
            res = afmf_framegen_descriptor_sets_create(dev, fg);
        if (res != VK_SUCCESS)
            blocker = "resource creation failed";
        else {
            afmf_framegen_dump_buffer_create(dev, fg);
            afmf_framegen_profiler_create(dev, fg);
        }
    }
    if (blocker != NULL) {
        AFMF_WARN("interpolation unavailable for this swapchain: %s (VkResult %d); repeating frames",
                  blocker, (int)res);
        afmf_framegen_destroy(dev, fg);
        return NULL;
    }
    AFMF_INFO("interpolation ready: %ux%u, flow at %ux%u (%ux%u blocks of %u px), %u pyramid "
              "levels, %s SAD%s%s",
              extent.width, extent.height, fg->of_extent.width, fg->of_extent.height,
              fg->flow_size[0].width, fg->flow_size[0].height, AFMF_FLOW_BLOCK * fg->flow_scale,
              fg->levels, dev->shader_int16 ? "packed 16-bit" : "scalar",
              fg->ingest_direct ? ", direct ingest" : "", fg->direct ? ", direct output" : "");
    return fg;
}

void afmf_framegen_set_diagnostics(struct afmf_framegen *fg,bool enabled)
{
    if(fg!=NULL) {
        bool before=fg->detailed;
        fg->diagnostics=enabled;
        const char *detail=getenv("GSF_NEXT_PROFILE_DETAIL");
        fg->detailed=enabled && !(detail!=NULL && strcmp(detail,"0")==0);
        if(before!=fg->detailed) {
            for(uint32_t k=0;k<3;k++) gsf_renderer_reset(&fg->choices[k],fg->choices[k].available);
            AFMF_INFO("GPU timing detail changed: %s",fg->detailed ? "detailed":"compact");
        }
    }
}

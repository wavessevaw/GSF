/* One frame's command buffer: the swapchain images through the layouts the work needs, around
 * the frame generation (framegen) or the plain copy of the history image. */

#include "swapchain_internal.h"

static void image_barrier(const struct afmf_device *dev, VkCommandBuffer cmd, VkImage image,
                          VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    dev->fns.cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* Records the companion for image i (frame N+1) into image j, and remembers frame N+1 for next
 * time. With interpolation: the optical flow and the interpolated frame (framegen.c). Without it:
 * a copy of the history image into j. Image i arrives in PRESENT_SRC (the application's
 * obligation) and is handed back in PRESENT_SRC. */
VkResult afmf_sc_record_frame(struct afmf_device *dev, struct afmf_swapchain *sc, VkCommandBuffer cmd,
                              uint32_t slot, uint32_t i, const uint32_t *targets, const float *phases, uint32_t count)
{
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = dev->fns.begin_command_buffer(cmd, &begin);
    if (res != VK_SUCCESS)
        return res;

    if(!sc->config.enabled) return dev->fns.end_command_buffer(cmd);

    const VkPipelineStageFlags top = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags bottom = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (sc->fg != NULL) {
        /* The target is written by a copy (TRANSFER_DST) or, with direct output, by the
         * interpolator's stores (GENERAL). */
        bool direct = afmf_framegen_direct(sc->fg);
        bool ingest = afmf_framegen_ingest_direct(sc->fg);
        const VkPipelineStageFlags compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkImageLayout written = direct ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkAccessFlags write = direct ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        VkPipelineStageFlags writer = direct ? compute : transfer;
        /* The current image is sampled by the ingest pass (GENERAL) or read by a copy. */
        VkImageLayout read = ingest ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAccessFlags read_access = ingest ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
        VkPipelineStageFlags reader = ingest ? compute : transfer;
        image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, read, 0, read_access,
                      top, reader);
        VkImage output_images[GSF_NEXT_MAX_OUTPUTS];
        uint32_t output_indices[GSF_NEXT_MAX_OUTPUTS];
        (void)targets; /* public output ordinals; descriptor indices address the private ring */
        /* All statically bound storage descriptors need GENERAL, even when count=2
         * makes the shader skip the third store. No unreserved output is computed. */
        uint32_t views = count > 1 && afmf_framegen_batched(sc->fg) ? 3u : count;
        for(uint32_t n=0;n<views;n++) {
            output_indices[n]=slot*GSF_NEXT_MAX_OUTPUTS+n;
            output_images[n]=sc->output_images[output_indices[n]];
            image_barrier(dev,cmd,output_images[n],VK_IMAGE_LAYOUT_UNDEFINED,written,0,write,top,writer);
        }
        afmf_framegen_record_multi(dev, sc->fg, cmd, slot, sc->images[i], i,
                                  output_images, output_indices, phases, count);
        /* A debug dump reads a direct output before this transition. Include its
         * transfer execution scope: layout transitions must follow readers too. */
        VkPipelineStageFlags completed = writer;
        if (afmf_framegen_dump_recorded(sc->fg)) completed |= transfer;
        for (uint32_t n = 0; n < count; n++)
            image_barrier(dev,cmd,output_images[n],written,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          write,VK_ACCESS_TRANSFER_READ_BIT,completed,transfer);
        image_barrier(dev, cmd, sc->images[i], read, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, read_access, 0,
                      reader, bottom);
        return dev->fns.end_command_buffer(cmd);
    }

    return VK_ERROR_FEATURE_NOT_PRESENT;
}

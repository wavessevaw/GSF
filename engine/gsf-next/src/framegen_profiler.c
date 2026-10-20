/* Frame generation: GPU timestamps per stage (AFMF_PROFILE=1). One query range per slot; a
 * slot's results are read back when the slot is reused, i.e. after its fence, so it never
 * blocks. */

#include "framegen_internal.h"
#include <unistd.h>

void afmf_framegen_profiler_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    if (!afmf_config_get()->profile)
        return;
    if (!dev->limits.timestampComputeAndGraphics || dev->limits.timestampPeriod <= 0.0f) {
        AFMF_WARN("profiling requested but the device has no timestamps on compute queues");
        return;
    }
    VkQueryPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = AFMF_PROFILE_QUERIES * fg->slots,
    };
    fg->query_pair = calloc(fg->slots,sizeof *fg->query_pair);
    fg->query_outputs = calloc(fg->slots,sizeof *fg->query_outputs);
    fg->query_phases = calloc((size_t)fg->slots*3,sizeof *fg->query_phases);
    fg->query_count = calloc(fg->slots, sizeof *fg->query_count);
    fg->query_stage = calloc((size_t)fg->slots * AFMF_PROFILE_QUERIES, sizeof *fg->query_stage);
    if (fg->query_pair == NULL || fg->query_outputs == NULL || fg->query_phases == NULL || fg->query_count == NULL || fg->query_stage == NULL ||
        dev->fns.create_query_pool(dev->handle, &pool, NULL, &fg->queries) != VK_SUCCESS) {
        AFMF_WARN("profiling unavailable: query pool creation failed");
        fg->queries = VK_NULL_HANDLE;
    }
}

/* Reads the timestamps a slot wrote the last time it ran and folds them into the stage totals.
 * Called once the slot's fence has been waited on, so the results are complete. */
void afmf_framegen_profiler_collect(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t slot)
{
    uint32_t count = fg->query_count[slot];
    fg->query_count[slot] = 0; /* consumed: never folded in twice */
    if (count < 2)
        return;
    uint64_t ticks[AFMF_PROFILE_QUERIES];
    VkResult res = dev->fns.get_query_pool_results(dev->handle, fg->queries,
                                                   slot * AFMF_PROFILE_QUERIES, count, sizeof ticks,
                                                   ticks, sizeof ticks[0], VK_QUERY_RESULT_64_BIT);
    if (res != VK_SUCCESS)
        return;
    fg->last_gpu_ms = (double)(ticks[count-1] - ticks[0]) * (double)dev->limits.timestampPeriod / 1e6;
    const uint8_t *stages = fg->query_stage + (size_t)slot * AFMF_PROFILE_QUERIES;
    fg->last_motion_ms = 0;
    fg->last_batch_ms = 0;
    double output_ms = 0;
    memset(fg->last_warp_ms,0,sizeof fg->last_warp_ms);
    uint32_t warp = 0;
    for (uint32_t i = 1; i < count; i++) {
        double ns = (double)(ticks[i]-ticks[i-1])*(double)dev->limits.timestampPeriod;
        fg->stage_ns[stages[i]] += ns;
        if (stages[i] == STAGE_INTERPOLATE) {
            if (warp < 3) fg->last_warp_ms[warp++] = ns/1e6;
        } else if (stages[i] == STAGE_BATCH) fg->last_batch_ms += ns/1e6;
        else if (stages[i] == STAGE_OUTPUT) output_ms += ns/1e6;
        else fg->last_motion_ms += ns/1e6;
    }
    const char *path = getenv("GSF_NEXT_GPU_REPORT");
    if (path != NULL && fg->diagnostics) {
        char filename[4096], backup[4096];
        int length=snprintf(filename,sizeof filename,"%s.%ld.%llu.jsonl",path,(long)getpid(),(unsigned long long)fg->context_id);
        FILE *out=NULL;
        if(length>0 && (size_t)length<sizeof filename) {
            struct stat st;
            if(stat(filename,&st)==0 && st.st_size>16*1024*1024) {
                int n=snprintf(backup,sizeof backup,"%s.1",filename);
                if(n>0 && (size_t)n<sizeof backup) (void)rename(filename,backup);
            }
            out=fopen(filename,"a");
        }
        if(out != NULL) {
            uint32_t pair=fg->query_pair[slot], outputs=fg->query_outputs[slot];
            (void)fprintf(out,"{\"batch_ms\":%.6f,\"scratch_copy_ms\":%.6f,\"direct_output\":%s,",
                fg->last_batch_ms,output_ms,fg->direct ? "true" : "false");
            (void)fprintf(out,"\"pair_b\":%u,\"pair_a\":%u,\"outputs\":%u,\"pair_ms\":%.6f,\"motion_ms\":%.6f,\"warp_ms\":[%.6f,%.6f,%.6f],\"phases\":[",
                pair,pair ? pair-1 : 0,outputs,fg->last_gpu_ms,fg->last_motion_ms,
                fg->last_warp_ms[0],fg->last_warp_ms[1],fg->last_warp_ms[2]);
            for(uint32_t k=0;k<outputs;k++)
                (void)fprintf(out,"%s%.6f", k ? ",":"",(double)fg->query_phases[(size_t)slot*3+k]);
            (void)fprintf(out,"]}\n");(void)fclose(out);
        }
    }
    fg->profiled_frames++;
}

void afmf_framegen_profiler_report(struct afmf_framegen *fg)
{
    if (fg->profiled_frames == 0)
        return;
    double total = 0.0;
    for (uint32_t s = 0; s < STAGE_COUNT; s++)
        total += fg->stage_ns[s];
    AFMF_INFO("GPU time per frame over %u frames at %ux%u: %.0f us total", fg->profiled_frames,
              fg->extent.width, fg->extent.height, total / fg->profiled_frames / 1e3);
    for (uint32_t s = 0; s < STAGE_COUNT; s++) {
        AFMF_INFO("  %-24s %7.0f us  %5.1f%%", stage_names[s],
                  fg->stage_ns[s] / fg->profiled_frames / 1e3,
                  total > 0.0 ? 100.0 * fg->stage_ns[s] / total : 0.0);
        fg->stage_ns[s] = 0.0;
    }
    fg->profiled_frames = 0;
}

/* Writes the timestamp that closes `stage`; a no-op without profiling. */
void afmf_framegen_profiler_mark(struct afmf_device *dev, struct afmf_framegen *fg,
                                 VkCommandBuffer cmd, uint32_t slot, enum stage stage)
{
    if (fg->queries == VK_NULL_HANDLE)
        return;
    uint32_t *count = &fg->query_count[slot];
    if (*count >= AFMF_PROFILE_QUERIES)
        return;
    fg->query_stage[(size_t)slot * AFMF_PROFILE_QUERIES + *count] = (uint8_t)stage;
    dev->fns.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fg->queries,
                                 slot * AFMF_PROFILE_QUERIES + *count);
    (*count)++;
}

void afmf_framegen_profiler_begin(struct afmf_device *dev, struct afmf_framegen *fg,
                                  VkCommandBuffer cmd, uint32_t slot)
{
    if (fg->queries == VK_NULL_HANDLE)
        return;
    afmf_framegen_profiler_collect(dev, fg, slot);
    if (fg->profiled_frames >= AFMF_PROFILE_INTERVAL)
        afmf_framegen_profiler_report(fg);
    dev->fns.cmd_reset_query_pool(cmd, fg->queries, slot * AFMF_PROFILE_QUERIES,
                                  AFMF_PROFILE_QUERIES);
    fg->query_pair[slot] = fg->frame_index;
    fg->query_count[slot] = 0;
    afmf_framegen_profiler_mark(dev, fg, cmd, slot, STAGE_INGEST); /* the opening timestamp; stage unused */
}

double afmf_framegen_gpu_ms(const struct afmf_framegen *fg)
{
    return fg != NULL && fg->queries != VK_NULL_HANDLE ? fg->last_gpu_ms : -1.0;
}

void afmf_framegen_gpu_stages(const struct afmf_framegen *fg, double *motion, double warps[3])
{
    *motion = fg != NULL ? fg->last_motion_ms : -1.0;
    for (uint32_t i=0;i<3;i++) warps[i] = fg != NULL ? fg->last_warp_ms[i] : -1.0;
}

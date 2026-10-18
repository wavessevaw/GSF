/* GFG Temporal Engine: Vulkan compute frame generation with temporal reuse.
 *
 * Contract (same shape as the MAKO backend the Frame Fusion layer uses):
 * two source images alternate (real frame n lives in slot n % 2), up to
 * TE_MAX_OUTPUTS destination images receive the synthetic frames of one pair,
 * and an optional timeline semaphore orders producer and consumer.  Images
 * and the semaphore are either created here (bench, tests) or imported from
 * opaque file descriptors (layer integration).  Synthetic output is never read
 * back as a reference: history holds only vectors and per-tile state.
 */
#ifndef TE_ENGINE_H
#define TE_ENGINE_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TE_VERSION "0.2.4-exp"
#define TE_MAX_OUTPUTS 4
#define TE_MAX_LEVELS 3

typedef struct te_device te_device;
typedef struct te_context te_context;

typedef struct {
    const char* device_name;   /* substring match; NULL picks the first device with compute */
    int validation;            /* enable VK_LAYER_KHRONOS_validation when present */
    int match_uuid;            /* only the physical device with this deviceUUID (same GPU as the exporter) */
    uint8_t device_uuid[VK_UUID_SIZE];
} te_device_desc;

typedef struct {
    uint32_t tile;             /* 8, 16 or 32 */
    uint32_t levels;           /* 1..TE_MAX_LEVELS */
    int32_t coarse_range;
    float me_bias;
    float static_thr, static_max_thr, light_thr, cut_thr, residual_thr;
    float occ_lo, occ_hi, smooth_bias;
    uint32_t probe;            /* 1..8 */
    uint32_t hysteresis;
    int32_t adaptive;          /* 0: every tile uses force_class */
    int32_t force_class;       /* 0 reuse, 1 light, 2 full */
    uint32_t full_density;     /* FULL decision per 1x1 or 2x2 block */
    uint32_t algorithm;        /* TE_ALGO_TILES, TE_ALGO_LMC (CPU fit), TE_ALGO_LMC_GPU */
} te_params;

enum { TE_ALGO_TILES = 0, TE_ALGO_LMC = 1, TE_ALGO_LMC_GPU = 2, TE_ALGO_SHARED_FLOW = 3 };
enum { TE_TRANSFER_CODED = 0, TE_TRANSFER_PQ = 1 };
#define TE_LMC_TIERS 3

void te_params_default(te_params* params);

typedef struct {
    uint32_t width, height;
    uint32_t outputs;          /* destination images, 1..TE_MAX_OUTPUTS */
    te_params params;
    int source_fds[2];         /* -1: allocate internally */
    int dest_fds[TE_MAX_OUTPUTS];
    int sync_fd;               /* -1: internal timeline semaphore */
    int export_fds;            /* internal images/semaphore are created exportable */
    int profile;               /* serialise executors so each one is timed on its own */
    int host_io;               /* create staging buffers for te_upload_rgb/te_download_rgb */
    int source_bgra;           /* source images are B8G8R8A8 (sampling keeps RGBA semantics) */
    VkSemaphore application_timeline; /* borrowed device: caller-owned, feature enabled by caller */
    int output_bgra;           /* tile outputs store BGRA bytes for GPU copy into a BGRA swapchain */
    VkFormat source_format;   /* 0: legacy SDR8; native GPU may sample packed UNORM10 */
    int shared_flow_dense;    /* experiment: 0 compact; 1 dense; 2 affine; 3 palette; 4 guarded projective fit */
    int output_fp16;          /* native HDR: rgba16f outputs, no intermediate 8-bit quantization */
    uint32_t color_transfer;  /* PQ: reconstruct in cached linear nits, return PQ-encoded FP16 */
} te_context_desc;

void te_context_desc_default(te_context_desc* desc, uint32_t width, uint32_t height, uint32_t outputs);

enum { TE_STAGE_PYRAMID, TE_STAGE_ANALYZE, TE_STAGE_CLASSIFY, TE_STAGE_COUNT };
/* LMC analysis passes, timed once per pair. TE_LMC_FIT_WAIT is a CPU-fit gap
 * only for TE_ALGO_LMC (excluded from GPU stage_ms). For TE_ALGO_LMC_GPU the
 * same slot times the GPU model solver and remains included in GPU totals. */
enum { TE_LMC_SEARCH_B, TE_LMC_SEARCH_A, TE_LMC_LAYERS, TE_LMC_FIT_WAIT, TE_LMC_FIELD, TE_LMC_MASKED,
       TE_LMC_ORPHAN, TE_LMC_FIELD2, TE_LMC_PROF_COUNT };
enum { TE_FRAME_LISTS, TE_FRAME_REUSE, TE_FRAME_LIGHT, TE_FRAME_FULL, TE_FRAME_TOTAL, TE_FRAME_COUNT };

typedef struct {
    uint64_t pair;                       /* pair counter (0 = first generated pair) */
    uint32_t outputs;
    int timed;                           /* GPU timestamps valid */
    int profiled;                        /* per-executor times valid (else -1) */
    int held;                            /* no previous frame: outputs copy the new real frame */
    uint64_t sequence;                   /* submission number of this pair (1 = first submit) */
    uint64_t submit_return_ns;           /* CLOCK_MONOTONIC after TE QueueSubmit */
    uint64_t gpu_start_ticks;            /* raw query 0, valid when timed */
    double gpu_timestamp_period_ns;
    uint32_t gpu_timestamp_valid_bits;
    uint64_t outputs_completed_total;    /* all outputs of this context whose GPU work finished */
    double stage_ms[TE_STAGE_COUNT];     /* once per pair */
    double lmc_ms[TE_LMC_PROF_COUNT];    /* LMC: per analysis pass (-1 when not timed) */
    double lmc_fit_ms;                   /* LMC: CPU time of this pair's motion-model fit */
    double ready_ms[TE_MAX_OUTPUTS];     /* analysis start to each output ready, includes fit/queue gaps */
    int shared_flow;                    /* experimental pair-shared backend, not a fitted global model */
    int shared_flow_fallback;           /* current pair requested full projective model after confidence test */
    uint32_t shared_flow_cut_bad_blocks; /* full-RGB compensated residual >0.15; dense GSF only */
    int source_duplicate;                 /* exact full-resolution RGB equality, GSF only */
    int model_gpu;                      /* LMC model/IRLS/palette execute on GPU, no fit worker */
    int lmc_tier;                        /* LMC: cost tier of the pair */
    double frame_ms[TE_MAX_OUTPUTS][TE_FRAME_COUNT];
    uint32_t tiles[TE_MAX_OUTPUTS][3];   /* reuse, light, full tile counts per output */
    uint32_t tile_total;
    uint32_t scene_cut;
    uint32_t promoted;                   /* promotions since context creation */
    double confidence;                   /* mean tile confidence of the pair */
} te_pair_stats;

typedef struct {
    uint32_t slot;              /* source slot that holds the new real frame */
    uint32_t count;             /* synthetic outputs to produce (0: history only) */
    float timestamps[TE_MAX_OUTPUTS]; /* strictly increasing in (0, 1) */
    uint64_t wait_value;        /* timeline value of the new frame; 0: no wait */
    uint64_t signal_first;      /* output i signals signal_first + i; count=0: analysis completion; 0: no signal */
    int nonblocking;            /* return VK_NOT_READY instead of waiting for a free slot */
    int download;               /* host_io: copy outputs to staging for te_download_rgb */
    int profile;                /* time each executor on its own for this pair */
    float light_scale;          /* scheduler: LIGHT threshold multiplier (0 means 1) */
    int force_full;             /* scheduler: classification off, every tile FULL */
    int force_light;            /* scheduler: cheapest level, FULL tiles run as LIGHT */
    int discontinuity;          /* the previous frame was skipped: slot may repeat, outputs hold */
    int hold;                   /* cost guard: no analysis, outputs copy the new real frame */
    int lmc_tier;               /* LMC cost tier: 0 full, 1 single grid (A from B), 2 also one
                                   propagation sweep (TE_LMC_TIERS) */
    int defer_second;           /* borrowed GPU GSF x3: queue analysis/G1 now; caller resumes G2 after G1 transport */
} te_submit;

const char* te_last_error(void);
/* Completes a deferred G2 submission without a CPU/GPU wait; no-op otherwise.
 * Poll/stats never resume implicitly. Blocking idle/destroy flush pending work.
 * Caller must keep the context/source images alive and synchronize its queue. */
VkResult te_resume_outputs(te_context* ctx);
/* Retire an unsubmitted G2 with its final signal/fence but no reconstruction.
 * Statistics/control then expose only G1. Safe for cut/duplicate/deadline drops. */
VkResult te_cancel_deferred_output(te_context* ctx);

VkResult te_device_create(const te_device_desc* desc, te_device** out);
void te_device_destroy(te_device* dev);
const char* te_device_name(const te_device* dev);
int te_device_supports_fd_import(const te_device* dev, int* memory, int* semaphore);
uint32_t te_device_validation_errors(const te_device* dev);

VkResult te_context_create(te_device* dev, const te_context_desc* desc, te_context** out);
/* Borrow a compute-capable application device/queue. No ownership transfer,
 * new device, timeline feature requirement or external-memory import. The
 * caller externally synchronizes ALL use of this queue. Tile or GPU LMC only.
 * Instance/device dispatch must refer to the SAME layer chain. Supply the
 * loader data callback when allocating below the loader, NULL at top level. */
typedef VkResult (VKAPI_PTR *te_set_device_loader_data)(VkDevice device, void* object);
VkResult te_device_borrow(VkInstance instance, PFN_vkGetInstanceProcAddr instance_dispatch, VkPhysicalDevice physical,
    VkDevice device, PFN_vkGetDeviceProcAddr device_dispatch, te_set_device_loader_data set_loader_data,
    VkQueue queue, uint32_t queue_family, te_device** out);
/* Borrowed contexts expose internal images in GENERAL layout for GPU copies.
 * Caller must order copies on the same queue and retain the context until
 * those copies finish. These functions never map or copy pixels to the CPU. */
VkImage te_source_image(te_context* ctx, uint32_t slot);
VkImage te_output_image(te_context* ctx, uint32_t output);
void te_device_timestamp_info(const te_device* dev, uint32_t* valid_bits, float* period_ns);
void te_context_destroy(te_context* ctx);

/* Upload a tightly packed RGB8 frame into a source slot (host_io contexts).
 * The copy is recorded at the start of the next submit for that slot. */
VkResult te_upload_rgb(te_context* ctx, uint32_t slot, const uint8_t* rgb);
VkResult te_submit_frame(te_context* ctx, const te_submit* submit);
/* Wait for everything submitted, then read the newest pair's statistics. */
VkResult te_wait_idle(te_context* ctx);
/* 1 when all submitted work completed within timeout_ns (0 polls), else 0. */
int te_context_idle(te_context* ctx, uint64_t timeout_ns);
/* Collect statistics of completed work without waiting. */
void te_poll(te_context* ctx);
uint64_t te_context_memory_bytes(const te_context* ctx);
/* Shared timeline (internal or imported): current value and host signal. */
uint64_t te_timeline_value(te_context* ctx);
VkResult te_timeline_signal(te_context* ctx, uint64_t value);
VkResult te_timeline_wait(te_context* ctx, uint64_t value, uint64_t timeout_ns);
/* Queue a timeline wait/signal probe on TE's own queue. Used only at bridge
 * startup to verify imported shared-FD payload ordering before WSI presents. */
VkResult te_timeline_fd_probe(te_context* ctx, uint64_t wait_value, uint64_t signal_value);
uint32_t te_context_outputs(const te_context* ctx);
/* Read the small coherent control header for one completed output of the
 * latest submission. ready_value must match that submission's timeline value;
 * this never waits for output 2, reads pixels, or collects incomplete queries. */
VkResult te_output_control(te_context* ctx,uint32_t output,uint64_t ready_value,
                         uint32_t* scene_cut,int* held,int* duplicate);
VkResult te_last_stats(te_context* ctx, te_pair_stats* stats);
VkResult te_download_rgb(te_context* ctx, uint32_t output, uint8_t* rgb);

/* Diagnostics copies of the newest pair (after te_wait_idle). */
uint32_t te_tile_count(const te_context* ctx, uint32_t* tx, uint32_t* ty);
VkResult te_read_vectors(te_context* ctx, float* xy);       /* tiles * 2 floats, level 0 */
VkResult te_read_classes(te_context* ctx, uint8_t* cls);    /* tiles: frame class of last output */
VkResult te_read_stats(te_context* ctx, float* stats8);     /* tiles * 8 floats */

/* Exported descriptors (export_fds contexts); caller owns the returned fd. */
int te_export_source_fd(te_context* ctx, uint32_t slot);
int te_export_dest_fd(te_context* ctx, uint32_t output);

#ifdef __cplusplus
}
#endif

#endif

/* GFG Temporal Engine: Vulkan host side.  See te_engine.h for the contract
 * and docs/TEMPORAL_ENGINE.md for the design. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES /* the loader is opened at run time, as MAKO does inside the layer */
#include "te_engine.h"
#include "te_lmc_fit.h"
#include "te_params.h"
#include "te_shaders.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


/* Opt-in monotonic HOST timings. These measure Vulkan host calls, not GPU time,
 * shader-cache hits or eventual WSI/physical scanout. Zero default overhead
 * on the frame submission path (only called at context initialization). */
static int te_profile_init_enabled(void) {
    const char* setting = getenv("GSF_TE_PROFILE_INIT");
    return setting && setting[0] == '1' && setting[1] == '\0';
}

static double te_profile_init_clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void te_profile_init_log(const char* phase, int pipeline, int variant,
                                double start_ms, VkResult result) {
    if (!te_profile_init_enabled()) return;
    fprintf(stderr, "[gfg-te] init_profile phase=%s pipeline=%d variant=%d host_ms=%.3f result=%d\n",
            phase, pipeline, variant, te_profile_init_clock_ms() - start_ms, (int)result);
}

#define TE_RING 2
#define TE_LMC_Q0 (4 + TE_MAX_OUTPUTS * 6)              /* LMC per-pass points */
#define TE_QUERIES (TE_LMC_Q0 + TE_LMC_PROF_COUNT + 1)
#define TE_GLOBALS_HEADER 96u
#define TE_LUMA_IMAGES (TE_MAX_LEVELS + 1)

enum {
    PIPE_LUMA, PIPE_DOWN, PIPE_ME, PIPE_STATS, PIPE_CLASSIFY, PIPE_LISTS,
    PIPE_REUSE, PIPE_LIGHT, PIPE_FULL, PIPE_COUNT
};

typedef struct {
    int32_t size[2];
    int32_t tiles[2];
    float static_thr, static_max_thr, light_thr, cut_thr, residual_thr;
    float occ_lo, occ_hi, smooth_bias, me_bias;
    int32_t coarse_range, hysteresis, levels, adaptive, force_class, tile, probe;
    int32_t full_density, pad[3];
    float level_size[4][4];
} te_ubo;
_Static_assert(offsetof(te_ubo, level_size) == 96, "std140 layout of Params");
_Static_assert(sizeof(te_ubo) == 160, "std140 size of Params");

typedef struct {
    float t;
    int32_t level, frame, parity;
    float step;
    int32_t flags;
} te_push;

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    VkDeviceSize alloc;
    void* map;
} te_buffer;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    uint32_t w, h;
    VkFormat fmt;
    VkDeviceSize alloc;
    int external;   /* imported or exportable: queue family transfers with EXTERNAL */
    int imported;
} te_image;

typedef struct {
    VkCommandBuffer analysis;
    VkCommandBuffer analysis2;   /* LMC: the second half, after the CPU motion fit */
    VkCommandBuffer out[TE_MAX_OUTPUTS];
    VkFence fence;
    VkQueryPool queries;
    te_buffer readback;        /* globals header after each output */
    int in_flight;
    uint64_t pair;
    uint32_t outputs;
    uint32_t parity;
    uint64_t sequence;
    uint64_t submit_return_ns;
    int generated;
    int held;
    int profiled;
    int lmc_timed;             /* LMC per-pass timestamps written */
    int lmc_tier;              /* LMC cost tier this pair was recorded at */
    int lmc_split;             /* LMC: analysis2 recorded (a generated pair) */
    /* LMC: the second half and the outputs, submitted by the fit worker once the model is in */
    VkSubmitInfo lsub[1 + TE_MAX_OUTPUTS];
    VkTimelineSemaphoreSubmitInfo ltsi[1 + TE_MAX_OUTPUTS];
    uint64_t lsig[TE_MAX_OUTPUTS];
    uint32_t lnsub;
    int lmc_pending;           /* LMC: the worker has not submitted lsub (nor the fence) yet */
    uint64_t lmc_seq;          /* LMC: the pair's fit number k (fit_tl 2k + 1, then 2k + 2) */
    double lmc_fit_ms;
    VkResult async_result;    /* the fit worker's second submission result */
    uint64_t deferred_first;  /* submitted G1 timeline value; error cleanup must not wait an unsent fence */
    uint64_t deferred_second;
    int deferred_failed;
} te_slot;

struct te_device {
    int borrowed;
    int subgroup_arithmetic;
    struct te_dispatch* dispatch;
    te_set_device_loader_data set_loader_data;
    VkInstance instance;
    VkPhysicalDevice phys;
    /* Borrowed handles belong to the next-layer dispatcher, not the global Vulkan loader. */
    PFN_vkGetPhysicalDeviceFormatProperties physical_format_properties;
    VkDevice device;
    VkQueue queue;
    uint32_t qfi;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    uint32_t ts_bits;
    int ext_mem_fd, ext_sem_fd;
    VkFormat luma_format;
    VkDebugUtilsMessengerEXT messenger;
    uint32_t validation_errors;
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    VkDeviceSize allocated;    /* bytes of device memory currently allocated by te */
    pthread_mutex_t queue_lock;   /* the queue is used by the submitting thread and the LMC fit worker */
    int queue_lock_ok;
};

struct te_lmc;

struct te_context {
    struct te_lmc* lmc;        /* TE_ALGO_LMC: resources of the layered motion path */
    te_device* dev;
    te_context_desc desc;
    uint32_t tx, ty, tiles;
    uint32_t lw[TE_LUMA_IMAGES], lh[TE_LUMA_IMAGES];
    te_image src[2], luma[2][TE_LUMA_IMAGES], dst[TE_MAX_OUTPUTS];
    te_buffer params, vectors[2], stats, states, residual, globals;
    te_buffer upload[2], download[TE_MAX_OUTPUTS], diag;
    int upload_pending[2];
    int upload_ring[2];        /* ring entry whose submit last read the staging buffer, -1 none */
    VkSampler sampler;
    VkDescriptorSetLayout dsl0, dsl1;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set0[2], set1[TE_MAX_OUTPUTS];
    VkPipeline pipe[PIPE_COUNT];
    VkCommandPool cmdpool;
    te_slot ring[TE_RING];
    uint32_t ring_next;
    te_slot *deferred_slot;
    VkSemaphore timeline;
    uint64_t frames;           /* real frames submitted */
    uint64_t pairs;            /* pairs with generated output */
    uint32_t last_slot;
    te_pair_stats last;
    int have_last;
    VkDeviceSize memory_bytes; /* device memory this context allocated (internal and imported) */
    uint64_t submits;          /* submissions so far */
    uint64_t output_ready[TE_MAX_OUTPUTS];
    uint64_t outputs_completed;/* outputs whose GPU work finished */
    uint64_t last_timing_sequence, last_submit_return_ns, last_gpu_start_ticks;
    double last_gpu_timestamp_period_ns;
    uint32_t last_gpu_timestamp_valid_bits;
};

static VkResult lmc_create(te_context* ctx);
static void lmc_destroy(te_context* ctx);
static void lmc_drain(te_context* ctx);
static int lmc_slot_pending(te_context* ctx, te_slot* slot);
static void lmc_record_init(te_context* ctx, VkCommandBuffer cb);

/* ------------------------------------------------------------------ loader */

#define TE_GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties)
#define TE_INSTANCE_FUNCS(X)                                                                                  \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                    \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties)                          \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)                          \
    X(vkGetPhysicalDeviceFeatures2) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)              \
    X(vkGetDeviceProcAddr)
#define TE_DEVICE_FUNCS(X)                                                                                    \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkAllocateMemory)       \
    X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer)                  \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage)             \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)       \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule)               \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)              \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)                      \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers)    \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence)                \
    X(vkDestroyFence) X(vkResetFences) X(vkGetFenceStatus) X(vkWaitForFences) X(vkCreateSemaphore)        \
    X(vkDestroySemaphore) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults)             \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch)                \
    X(vkCmdDispatchIndirect) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage)         \
    X(vkCmdCopyImageToBuffer) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdClearColorImage)            \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetSemaphoreCounterValue) X(vkSignalSemaphore) X(vkWaitSemaphores)

#define TE_DECLARE(name) static PFN_##name name;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
TE_GLOBAL_FUNCS(TE_DECLARE)
TE_INSTANCE_FUNCS(TE_DECLARE)
#define TE_DECLARE_DEVICE(name) static __thread PFN_##name name;
TE_DEVICE_FUNCS(TE_DECLARE_DEVICE)
#define TE_DISPATCH_FIELD(name) PFN_##name name;
typedef struct te_dispatch { TE_DEVICE_FUNCS(TE_DISPATCH_FIELD) } te_dispatch;
static te_dispatch g_loader_device_dispatch;
static __thread te_device* g_active_device;

static VkResult VKAPI_CALL borrowed_allocate_commands(VkDevice device,
        const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    te_device* dev = g_active_device;
    VkResult r = dev->dispatch->vkAllocateCommandBuffers(device, info, buffers);
    if (r == VK_SUCCESS && dev->set_loader_data) {
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
            r = dev->set_loader_data(device, buffers[i]);
            if (r != VK_SUCCESS) {
                dev->dispatch->vkFreeCommandBuffers(device, info->commandPool, info->commandBufferCount, buffers);
                return r;
            }
        }
    }
    return r;
}

/* Each application device can sit at a different point in the layer chain.
 * Select its dispatch on every API entry, including worker threads. */
static void select_device(te_device* dev) {
    const te_dispatch* dispatch = dev && dev->dispatch ? dev->dispatch : &g_loader_device_dispatch;
#define TE_SELECT_DEVICE(name) name = dispatch->name;
    TE_DEVICE_FUNCS(TE_SELECT_DEVICE)
    g_active_device = dev;
    if (dev && dev->dispatch && dev->set_loader_data)
        vkAllocateCommandBuffers = borrowed_allocate_commands;
}

static pthread_once_t g_global_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_instance_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_global_ok, g_instance_ok;

static void load_global(void) {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) return;
    int ok = 1;
#define TE_LOAD_GLOBAL(name) ok &= (name = (PFN_##name)vkGetInstanceProcAddr(NULL, #name)) != NULL;
    TE_GLOBAL_FUNCS(TE_LOAD_GLOBAL)
    g_global_ok = ok;
}

/* Standalone devices use loader trampolines. Borrowed devices select the
 * caller's next-layer device dispatch, keeping wrapped handles consistent. */
static int load_instance(VkInstance instance) {
    pthread_mutex_lock(&g_instance_lock);
    if (!g_instance_ok) {
        int ok = 1;
#define TE_LOAD_INSTANCE(name) ok &= (name = (PFN_##name)vkGetInstanceProcAddr(instance, #name)) != NULL;
        TE_INSTANCE_FUNCS(TE_LOAD_INSTANCE)
#define TE_LOAD_DEVICE(name) ok &= (g_loader_device_dispatch.name = (PFN_##name)vkGetInstanceProcAddr(instance, #name)) != NULL;
        TE_DEVICE_FUNCS(TE_LOAD_DEVICE)
        g_instance_ok = ok;
    }
    int ok = g_instance_ok;
    pthread_mutex_unlock(&g_instance_lock);
    select_device(NULL);
    return ok;
}

/* ------------------------------------------------------------------ errors */

static __thread char g_error[512];

const char* te_last_error(void) { return g_error; }

static VkResult fail(VkResult res, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
    return res;
}

/* every submission of the engine's queue (the submitting thread and the LMC fit worker) */
static VkResult queue_submit(te_device* dev, uint32_t n, const VkSubmitInfo* si, VkFence fence) {
    select_device(dev);
    pthread_mutex_lock(&dev->queue_lock);
    VkResult r = vkQueueSubmit(dev->queue, n, si, fence);
    pthread_mutex_unlock(&dev->queue_lock);
    return r;
}

#define CHECK(call, what)                                                     \
    do {                                                                      \
        VkResult r_ = (call);                                                 \
        if (r_ != VK_SUCCESS) return fail(r_, "%s failed (%d)", what, r_);    \
    } while (0)

/* ------------------------------------------------------------------ params */

void te_params_default(te_params* p) {
    memset(p, 0, sizeof *p);
    p->tile = TE_DEFAULT_TILE;
    p->levels = TE_DEFAULT_LEVELS;
    p->coarse_range = TE_DEFAULT_COARSE_RANGE;
    p->me_bias = TE_DEFAULT_ME_BIAS;
    p->static_thr = TE_DEFAULT_STATIC_THR;
    p->static_max_thr = TE_DEFAULT_STATIC_MAX_THR;
    p->light_thr = TE_DEFAULT_LIGHT_THR;
    p->cut_thr = TE_DEFAULT_CUT_THR;
    p->residual_thr = TE_DEFAULT_RESIDUAL_THR;
    p->occ_lo = TE_DEFAULT_OCC_LO;
    p->occ_hi = TE_DEFAULT_OCC_HI;
    p->smooth_bias = TE_DEFAULT_SMOOTH_BIAS;
    p->probe = TE_DEFAULT_PROBE;
    p->hysteresis = TE_DEFAULT_HYSTERESIS;
    p->adaptive = 1;
    p->force_class = 2;
    p->full_density = TE_DEFAULT_FULL_DENSITY;
    p->algorithm = TE_ALGO_TILES;
}

void te_context_desc_default(te_context_desc* d, uint32_t width, uint32_t height, uint32_t outputs) {
    memset(d, 0, sizeof *d);
    d->width = width;
    d->height = height;
    d->outputs = outputs;
    te_params_default(&d->params);
    d->source_fds[0] = d->source_fds[1] = -1;
    for (int i = 0; i < TE_MAX_OUTPUTS; ++i) d->dest_fds[i] = -1;
    d->sync_fd = -1;
}

/* ------------------------------------------------------------------ device */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)type;
    te_device* dev = user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        dev->validation_errors++;
        fprintf(stderr, "te validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static int has_layer(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties* props = calloc(n ? n : 1, sizeof *props);
    vkEnumerateInstanceLayerProperties(&n, props);
    int found = 0;
    for (uint32_t i = 0; i < n; ++i) found |= strcmp(props[i].layerName, name) == 0;
    free(props);
    return found;
}

static int has_device_ext(VkPhysicalDevice phys, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    VkExtensionProperties* props = calloc(n ? n : 1, sizeof *props);
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, props);
    int found = 0;
    for (uint32_t i = 0; i < n; ++i) found |= strcmp(props[i].extensionName, name) == 0;
    free(props);
    return found;
}

static int format_ok(const te_device *dev, VkFormat fmt, VkFormatFeatureFlags need) {
    /* The loader's global physical-device trampoline ABORTS with a borrowed
     * lower-layer handle under Gamescope. Use the matching layer dispatcher. */
    PFN_vkGetPhysicalDeviceFormatProperties query = dev->borrowed
        ? dev->physical_format_properties : vkGetPhysicalDeviceFormatProperties;
    if (!query) return 0;
    VkFormatProperties fp = {0};
    query(dev->phys, fmt, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

VkResult te_device_create(const te_device_desc* desc, te_device** out) {
    *out = NULL;
    pthread_once(&g_global_once, load_global);
    if (!g_global_ok) return fail(VK_ERROR_INITIALIZATION_FAILED, "libvulkan.so.1 could not be loaded");
    te_device* dev = calloc(1, sizeof *dev);
    if (!dev) return fail(VK_ERROR_OUT_OF_HOST_MEMORY, "out of memory");
    int validation = desc && desc->validation && has_layer("VK_LAYER_KHRONOS_validation");
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* inst_ext[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "gfg-temporal",
                             .applicationVersion = 1, .pEngineName = "gfg-temporal", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledLayerCount = validation ? 1u : 0u, .ppEnabledLayerNames = layers,
                                .enabledExtensionCount = validation ? 1u : 0u, .ppEnabledExtensionNames = inst_ext};
    VkResult r = vkCreateInstance(&ici, NULL, &dev->instance);
    if (r != VK_SUCCESS) { free(dev); return fail(r, "vkCreateInstance failed (%d)", r); }
    if (!load_instance(dev->instance)) {
        te_device_destroy(dev);
        return fail(VK_ERROR_INITIALIZATION_FAILED, "Vulkan entry points are missing");
    }
    if (validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(dev->instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mci = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            .pfnUserCallback = debug_cb, .pUserData = dev};
        if (create) create(dev->instance, &mci, NULL, &dev->messenger);
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(dev->instance, &n, NULL);
    VkPhysicalDevice* phys = calloc(n ? n : 1, sizeof *phys);
    vkEnumeratePhysicalDevices(dev->instance, &n, phys);
    dev->qfi = UINT32_MAX;
    for (uint32_t i = 0; i < n && dev->qfi == UINT32_MAX; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys[i], &props);
        if (desc && desc->device_name && !strstr(props.deviceName, desc->device_name)) continue;
        if (desc && desc->match_uuid) {
            VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
            vkGetPhysicalDeviceProperties2(phys[i], &p2);
            if (memcmp(id.deviceUUID, desc->device_uuid, VK_UUID_SIZE) != 0) continue;
        }
        if (props.apiVersion < VK_API_VERSION_1_2) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, NULL);
        VkQueueFamilyProperties* qp = calloc(qn ? qn : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, qp);
        for (uint32_t q = 0; q < qn; ++q) {
            if (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                dev->phys = phys[i];
                dev->qfi = q;
                dev->ts_bits = qp[q].timestampValidBits;
                dev->props = props;
                break;
            }
        }
        free(qp);
    }
    free(phys);
    if (dev->qfi == UINT32_MAX) {
        te_device_destroy(dev);
        return fail(VK_ERROR_INITIALIZATION_FAILED, "no Vulkan 1.2 device with a compute queue%s%s%s",
                    desc && desc->device_name ? " matching " : "", desc && desc->device_name ? desc->device_name : "",
                    desc && desc->match_uuid ? " with the requested device UUID" : "");
    }
    vkGetPhysicalDeviceMemoryProperties(dev->phys, &dev->mem);
    VkPhysicalDeviceSubgroupProperties subgroup={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 subgroup_props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&subgroup};
    vkGetPhysicalDeviceProperties2(dev->phys,&subgroup_props);
    dev->subgroup_arithmetic=(subgroup.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT) &&
        (subgroup.supportedOperations&VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) && subgroup.subgroupSize>=4;
    const VkFormatFeatureFlags luma_need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (format_ok(dev, VK_FORMAT_R16_SFLOAT, luma_need)) dev->luma_format = VK_FORMAT_R16_SFLOAT;
    else if (format_ok(dev, VK_FORMAT_R32_SFLOAT, luma_need)) dev->luma_format = VK_FORMAT_R32_SFLOAT;
    else {
        te_device_destroy(dev);
        return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "no filterable storage format for the luma pyramid");
    }
    if (!format_ok(dev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        te_device_destroy(dev);
        return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "RGBA8 images are not storage/filterable on this device");
    }
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(dev->phys, &f2);
    if (!f12.timelineSemaphore) {
        te_device_destroy(dev);
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "timeline semaphores are not supported");
    }
    const char* ext[4];
    uint32_t next = 0;
    dev->ext_mem_fd = has_device_ext(dev->phys, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    dev->ext_sem_fd = has_device_ext(dev->phys, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    if (dev->ext_mem_fd) ext[next++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    if (dev->ext_sem_fd) ext[next++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = dev->qfi,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan12Features en12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                             .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &en12, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = next, .ppEnabledExtensionNames = ext};
    r = vkCreateDevice(dev->phys, &dci, NULL, &dev->device);
    if (r != VK_SUCCESS) {
        te_device_destroy(dev);
        return fail(r, "vkCreateDevice failed (%d)", r);
    }
    vkGetDeviceQueue(dev->device, dev->qfi, 0, &dev->queue);
    pthread_mutex_init(&dev->queue_lock, NULL);
    dev->queue_lock_ok = 1;
    if (dev->ext_mem_fd)
        dev->get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev->device, "vkGetMemoryFdKHR");
    if (dev->ext_sem_fd) {
        dev->get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(dev->device, "vkGetSemaphoreFdKHR");
        dev->import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(dev->device, "vkImportSemaphoreFdKHR");
    }
    *out = dev;
    return VK_SUCCESS;
}

void te_device_destroy(te_device* dev) {
    if (!dev) return;
    select_device(dev);
    if (dev->device && !dev->borrowed) vkDestroyDevice(dev->device, NULL);
    if (dev->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(dev->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(dev->instance, dev->messenger, NULL);
    }
    if (dev->instance && !dev->borrowed) vkDestroyInstance(dev->instance, NULL);
    if (dev->queue_lock_ok) pthread_mutex_destroy(&dev->queue_lock);
    free(dev->dispatch);
    free(dev);
    select_device(NULL);
}

const char* te_device_name(const te_device* dev) { return dev->props.deviceName; }

VkResult te_device_borrow(VkInstance instance, PFN_vkGetInstanceProcAddr instance_dispatch, VkPhysicalDevice physical,
        VkDevice device, PFN_vkGetDeviceProcAddr device_dispatch, te_set_device_loader_data set_loader_data,
        VkQueue queue, uint32_t family, te_device** out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = NULL;
    if (!instance || !instance_dispatch || !physical || !device || !device_dispatch || !queue) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_once(&g_global_once, load_global);
    if (!g_global_ok || !load_instance(instance))
        return fail(VK_ERROR_INITIALIZATION_FAILED, "application Vulkan 1.2 dispatch unavailable");
    /* Layer callbacks receive a physical handle owned by the next dispatch
     * chain. Never pass it to the loader's top-level physical trampolines. */
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        instance_dispatch(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceFormatProperties formats = (PFN_vkGetPhysicalDeviceFormatProperties)
        instance_dispatch(instance,"vkGetPhysicalDeviceFormatProperties");
    PFN_vkGetPhysicalDeviceProperties properties = (PFN_vkGetPhysicalDeviceProperties)
        instance_dispatch(instance,"vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memory = (PFN_vkGetPhysicalDeviceMemoryProperties)
        instance_dispatch(instance,"vkGetPhysicalDeviceMemoryProperties");
    if(!families || !formats || !properties || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    families(physical, &count, NULL);
    if (family >= count) return fail(VK_ERROR_INITIALIZATION_FAILED, "invalid application queue family");
    VkQueueFamilyProperties* qp = calloc(count, sizeof(*qp));
    if (!qp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    families(physical, &count, qp);
    VkQueueFamilyProperties selected = qp[family];
    free(qp);
    if (!(selected.queueFlags & VK_QUEUE_COMPUTE_BIT))
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "presentation queue cannot execute compute");
    if (!selected.timestampValidBits)
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "compute timestamps unavailable on application queue");
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    VkFormatProperties fp0,fp1;
    formats(physical,VK_FORMAT_R32_SFLOAT,&fp0);
    formats(physical,VK_FORMAT_R8G8B8A8_UNORM,&fp1);
    if ((fp0.optimalTilingFeatures & required)!=required ||
        (fp1.optimalTilingFeatures & required)!=required)
        return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "application GPU lacks required storage formats");
    te_device* dev = calloc(1, sizeof(*dev));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;
    dev->dispatch = calloc(1, sizeof(*dev->dispatch));
    if (!dev->dispatch) { free(dev); return VK_ERROR_OUT_OF_HOST_MEMORY; }
#define TE_BORROW_DEVICE(name) dev->dispatch->name = (PFN_##name)device_dispatch(device, #name);
    TE_DEVICE_FUNCS(TE_BORROW_DEVICE)
    dev->set_loader_data = set_loader_data;
    dev->borrowed = 1;
    dev->instance = instance;
    dev->phys = physical;
    dev->physical_format_properties = formats;
    dev->device = device;
    dev->queue = queue;
    dev->qfi = family;
    dev->ts_bits = selected.timestampValidBits;
    /* R32F needs no shaderStorageImageExtendedFormats application feature. */
    dev->luma_format = VK_FORMAT_R32_SFLOAT;
    properties(physical, &dev->props);
    memory(physical, &dev->mem);
    PFN_vkGetPhysicalDeviceProperties2 properties2=(PFN_vkGetPhysicalDeviceProperties2)
        instance_dispatch(instance,"vkGetPhysicalDeviceProperties2");
    if(properties2){
        VkPhysicalDeviceSubgroupProperties subgroup={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 props2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&subgroup};
        properties2(physical,&props2);
        dev->subgroup_arithmetic=(subgroup.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT) &&
            (subgroup.supportedOperations&VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) && subgroup.subgroupSize>=4;
    }
    if (pthread_mutex_init(&dev->queue_lock, NULL) != 0) { free(dev->dispatch); free(dev); return VK_ERROR_INITIALIZATION_FAILED; }
    dev->queue_lock_ok = 1;
    *out = dev;
    return VK_SUCCESS;
}

int te_device_supports_fd_import(const te_device* dev, int* memory, int* semaphore) {
    if (memory) *memory = dev->ext_mem_fd;
    if (semaphore) *semaphore = dev->ext_sem_fd;
    return dev->ext_mem_fd && dev->ext_sem_fd;
}

uint32_t te_device_validation_errors(const te_device* dev) { return dev->validation_errors; }

/* ------------------------------------------------------------------ memory */

static int32_t find_memory(te_device* dev, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < dev->mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (dev->mem.memoryTypes[i].propertyFlags & want) == want) return (int32_t)i;
    return -1;
}

static VkResult buffer_create(te_device* dev, te_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage, int host) {
    memset(b, 0, sizeof *b);
    b->size = size;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(dev->device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev->device, b->buf, &req);
    int32_t type = host ? find_memory(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                        : find_memory(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0 && !host) type = find_memory(dev, req.memoryTypeBits, 0);
    if (type < 0) return fail(VK_ERROR_OUT_OF_DEVICE_MEMORY, "no memory type for a buffer");
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = (uint32_t)type};
    CHECK(vkAllocateMemory(dev->device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    b->alloc = req.size;
    dev->allocated += req.size;
    CHECK(vkBindBufferMemory(dev->device, b->buf, b->mem, 0), "vkBindBufferMemory");
    if (host) CHECK(vkMapMemory(dev->device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map), "vkMapMemory");
    return VK_SUCCESS;
}

static void buffer_destroy(te_device* dev, te_buffer* b) {
    if (b->map) vkUnmapMemory(dev->device, b->mem);
    if (b->buf) vkDestroyBuffer(dev->device, b->buf, NULL);
    if (b->mem) vkFreeMemory(dev->device, b->mem, NULL);
    dev->allocated -= b->alloc;
    memset(b, 0, sizeof *b);
}

/* fd >= 0 imports (ownership passes to Vulkan on success); export marks the
 * memory exportable as an opaque fd. */
static VkResult image_create(te_device* dev, te_image* im, uint32_t w, uint32_t h, VkFormat fmt,
                             VkImageUsageFlags usage, int fd, int export_fd) {
    memset(im, 0, sizeof *im);
    im->w = w;
    im->h = h;
    im->fmt = fmt;
    im->external = fd >= 0 || export_fd;
    im->imported = fd >= 0;
    if (im->external && !dev->ext_mem_fd) {
        if (fd >= 0) close(fd);
        return fail(VK_ERROR_EXTENSION_NOT_PRESENT, "VK_KHR_external_memory_fd is not supported");
    }
    VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = im->external ? &emi : NULL,
                             .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = 1,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vkCreateImage(dev->device, &ici, NULL, &im->img);
    if (r != VK_SUCCESS) {
        if (fd >= 0) close(fd);
        return fail(r, "vkCreateImage failed (%d)", r);
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev->device, im->img, &req);
    int32_t type = find_memory(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(dev, req.memoryTypeBits, 0);
    if (type < 0) {
        if (fd >= 0) close(fd);
        return fail(VK_ERROR_OUT_OF_DEVICE_MEMORY, "no memory type for an image");
    }
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = im->img};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd};
    VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &ded,
                                      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .pNext = fd >= 0 ? (void*)&imp : (export_fd ? (void*)&exp : NULL),
                                .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type};
    r = vkAllocateMemory(dev->device, &mai, NULL, &im->mem);
    if (r != VK_SUCCESS) {
        if (fd >= 0) close(fd);
        return fail(r, "vkAllocateMemory(image%s) failed (%d)", fd >= 0 ? " import" : "", r);
    }
    im->alloc = req.size;
    dev->allocated += req.size;
    CHECK(vkBindImageMemory(dev->device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(dev->device, &vci, NULL, &im->view), "vkCreateImageView");
    return VK_SUCCESS;
}

static void image_destroy(te_device* dev, te_image* im) {
    if (im->view) vkDestroyImageView(dev->device, im->view, NULL);
    if (im->img) vkDestroyImage(dev->device, im->img, NULL);
    if (im->mem) vkFreeMemory(dev->device, im->mem, NULL);
    dev->allocated -= im->alloc;
    memset(im, 0, sizeof *im);
}

/* ------------------------------------------------------------------ barriers */

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst,
                    VkAccessFlags da) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da};
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void compute_barrier(VkCommandBuffer cb) {
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

static void ownership(VkCommandBuffer cb, te_image* im, uint32_t qfi, int acquire) {
    if (!im->external) return;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : 0,
                              .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : qfi,
                              .dstQueueFamilyIndex = acquire ? qfi : VK_QUEUE_FAMILY_EXTERNAL, .image = im->img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                         0, NULL, 1, &b);
}

/* ------------------------------------------------------------------ pipelines */

static VkResult make_pipeline(te_context* ctx, const uint32_t* code, size_t bytes, VkPipeline* out) {
    VkDevice d = ctx->dev->device;
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes,
                                     .pCode = code};
    VkShaderModule mod;
    double module_started = te_profile_init_clock_ms();
    VkResult module_result = vkCreateShaderModule(d, &smci, NULL, &mod);
    te_profile_init_log("temporal-shader-module", -1, -1, module_started, module_result);
    CHECK(module_result, "vkCreateShaderModule");
    VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"},
                                        .layout = ctx->layout};
    double pipeline_started = te_profile_init_clock_ms();
    VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpci, NULL, out);
    te_profile_init_log("temporal-compute-pipeline", -1, -1, pipeline_started, r);
    vkDestroyShaderModule(d, mod, NULL);
    if (r != VK_SUCCESS) return fail(r, "vkCreateComputePipelines failed (%d)", r);
    return VK_SUCCESS;
}

static VkResult create_layouts(te_context* ctx) {
    VkDevice d = ctx->dev->device;
    VkDescriptorSetLayoutBinding b0[21];
    for (uint32_t i = 0; i < 21; ++i) {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i == 0) type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (i <= 10) type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (i >= 17) type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b0[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = type, .descriptorCount = 1,
                                               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    VkDescriptorSetLayoutCreateInfo l0 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 21, .pBindings = b0};
    CHECK(vkCreateDescriptorSetLayout(d, &l0, NULL, &ctx->dsl0), "vkCreateDescriptorSetLayout(0)");
    VkDescriptorSetLayoutBinding b1 = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo l1 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1, .pBindings = &b1};
    CHECK(vkCreateDescriptorSetLayout(d, &l1, NULL, &ctx->dsl1), "vkCreateDescriptorSetLayout(1)");
    VkDescriptorSetLayout sets[2] = {ctx->dsl0, ctx->dsl1};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(te_push)};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2,
                                       .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    CHECK(vkCreatePipelineLayout(d, &plci, NULL, &ctx->layout), "vkCreatePipelineLayout");
    VkDescriptorPoolSize sizes[4] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 20},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 + TE_MAX_OUTPUTS},
    };
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 2 + TE_MAX_OUTPUTS, .poolSizeCount = 4, .pPoolSizes = sizes};
    CHECK(vkCreateDescriptorPool(d, &dpci, NULL, &ctx->pool), "vkCreateDescriptorPool");
    return VK_SUCCESS;
}

static VkResult create_pipelines(te_context* ctx) {
    int r16 = ctx->dev->luma_format == VK_FORMAT_R16_SFLOAT;
    int fp16 = ctx->desc.output_fp16;
    struct { const uint32_t* code; size_t size; } src[PIPE_COUNT] = {
        [PIPE_LUMA] = {r16 ? te_spv_luma_r16f : te_spv_luma_r32f, r16 ? sizeof te_spv_luma_r16f : sizeof te_spv_luma_r32f},
        [PIPE_DOWN] = {r16 ? te_spv_down_r16f : te_spv_down_r32f, r16 ? sizeof te_spv_down_r16f : sizeof te_spv_down_r32f},
        [PIPE_ME] = {te_spv_me, sizeof te_spv_me},
        [PIPE_STATS] = {te_spv_stats, sizeof te_spv_stats},
        [PIPE_CLASSIFY] = {te_spv_classify, sizeof te_spv_classify},
        [PIPE_LISTS] = {te_spv_lists, sizeof te_spv_lists},
        [PIPE_REUSE] = {fp16 ? te_spv_reuse_fp16 : te_spv_reuse, fp16 ? sizeof te_spv_reuse_fp16 : sizeof te_spv_reuse},
        [PIPE_LIGHT] = {fp16 ? te_spv_light_fp16 : te_spv_light, fp16 ? sizeof te_spv_light_fp16 : sizeof te_spv_light},
        [PIPE_FULL] = {fp16 ? te_spv_full_fp16 : te_spv_full, fp16 ? sizeof te_spv_full_fp16 : sizeof te_spv_full},
    };
    for (int i = 0; i < PIPE_COUNT; ++i) {
        double stage_started = te_profile_init_clock_ms();
        VkResult r = make_pipeline(ctx, src[i].code, src[i].size, &ctx->pipe[i]);
        te_profile_init_log("temporal-pipeline-total", i, 0, stage_started, r);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

static void write_sets(te_context* ctx) {
    VkDevice d = ctx->dev->device;
    for (uint32_t par = 0; par < 2; ++par) {
        uint32_t a = par ^ 1u, b = par;
        VkDescriptorBufferInfo ubo = {ctx->params.buf, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo img[20];
        img[0] = (VkDescriptorImageInfo){ctx->sampler, ctx->src[a].view, VK_IMAGE_LAYOUT_GENERAL};
        img[1] = (VkDescriptorImageInfo){ctx->sampler, ctx->src[b].view, VK_IMAGE_LAYOUT_GENERAL};
        for (int l = 0; l < TE_LUMA_IMAGES; ++l) {
            img[2 + l] = (VkDescriptorImageInfo){ctx->sampler, ctx->luma[a][l].view, VK_IMAGE_LAYOUT_GENERAL};
            img[6 + l] = (VkDescriptorImageInfo){ctx->sampler, ctx->luma[b][l].view, VK_IMAGE_LAYOUT_GENERAL};
            img[10 + l] = (VkDescriptorImageInfo){VK_NULL_HANDLE, ctx->luma[b][l].view, VK_IMAGE_LAYOUT_GENERAL};
        }
        VkDescriptorBufferInfo bufs[6] = {
            {ctx->vectors[par].buf, 0, VK_WHOLE_SIZE}, {ctx->vectors[par ^ 1u].buf, 0, VK_WHOLE_SIZE},
            {ctx->stats.buf, 0, VK_WHOLE_SIZE},        {ctx->states.buf, 0, VK_WHOLE_SIZE},
            {ctx->residual.buf, 0, VK_WHOLE_SIZE},     {ctx->globals.buf, 0, VK_WHOLE_SIZE},
        };
        VkWriteDescriptorSet w[21];
        for (uint32_t i = 0; i < 21; ++i) {
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ctx->set0[par],
                                          .dstBinding = i, .descriptorCount = 1};
            if (i == 0) {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w[i].pBufferInfo = &ubo;
            } else if (i <= 10) {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &img[i - 1];
            } else if (i <= 16) {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[i].pBufferInfo = &bufs[i - 11];
            } else {
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w[i].pImageInfo = &img[10 + (i - 17)];
            }
        }
        vkUpdateDescriptorSets(d, 21, w, 0, NULL);
    }
    for (uint32_t i = 0; i < ctx->desc.outputs; ++i) {
        VkDescriptorImageInfo di = {VK_NULL_HANDLE, ctx->dst[i].view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ctx->set1[i],
                                  .dstBinding = 0, .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &di};
        vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
    }
}

/* ------------------------------------------------------------------ context */

static VkResult one_time(te_context* ctx, void (*record)(te_context*, VkCommandBuffer, void*), void* user) {
    select_device(ctx->dev);
    VkDevice d = ctx->dev->device;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = ctx->cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(d, &cai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    record(ctx, cb, user);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(d, &fci, NULL, &fence), "vkCreateFence");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VkResult r = queue_submit(ctx->dev, 1, &si, fence);
    if (r == VK_SUCCESS) r = vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(d, fence, NULL);
    vkFreeCommandBuffers(d, ctx->cmdpool, 1, &cb);
    if (r != VK_SUCCESS) return fail(r, "one-time submit failed (%d)", r);
    return VK_SUCCESS;
}

static void record_init(te_context* ctx, VkCommandBuffer cb, void* user) {
    (void)user;
    te_image* all[2 + 2 * TE_LUMA_IMAGES + TE_MAX_OUTPUTS];
    uint32_t n = 0;
    all[n++] = &ctx->src[0];
    all[n++] = &ctx->src[1];
    for (int s = 0; s < 2; ++s)
        for (int l = 0; l < TE_LUMA_IMAGES; ++l) all[n++] = &ctx->luma[s][l];
    for (uint32_t i = 0; i < ctx->desc.outputs; ++i) all[n++] = &ctx->dst[i];
    VkImageMemoryBarrier b[2 + 2 * TE_LUMA_IMAGES + TE_MAX_OUTPUTS];
    for (uint32_t i = 0; i < n; ++i)
        b[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, /* no content yet */
                                      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = all[i]->img,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, n, b);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int s = 0; s < 2; ++s)
        for (int l = 0; l < TE_LUMA_IMAGES; ++l)
            vkCmdClearColorImage(cb, ctx->luma[s][l].img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    te_buffer* zeroed[] = {&ctx->vectors[0], &ctx->vectors[1], &ctx->stats, &ctx->states, &ctx->residual,
                           &ctx->globals};
    for (size_t i = 0; i < sizeof zeroed / sizeof zeroed[0]; ++i)
        vkCmdFillBuffer(cb, zeroed[i]->buf, 0, VK_WHOLE_SIZE, 0);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (ctx->lmc) lmc_record_init(ctx, cb);
}

static VkResult import_timeline(te_context* ctx, int fd) {
    te_device* dev = ctx->dev;
    VkExportSemaphoreCreateInfo esci = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkSemaphoreTypeCreateInfo stci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .pNext = (fd >= 0 || ctx->desc.export_fds) && dev->ext_sem_fd ? &esci : NULL,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &stci};
    if (fd >= 0 && !dev->ext_sem_fd) {
        close(fd);
        return fail(VK_ERROR_EXTENSION_NOT_PRESENT, "VK_KHR_external_semaphore_fd is not supported on %s",
                    dev->props.deviceName);
    }
    VkResult r = vkCreateSemaphore(dev->device, &sci, NULL, &ctx->timeline);
    if (r != VK_SUCCESS) {
        if (fd >= 0) close(fd);
        return fail(r, "vkCreateSemaphore(timeline) failed (%d)", r);
    }
    if (fd >= 0) {
        VkImportSemaphoreFdInfoKHR info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                                           .semaphore = ctx->timeline,
                                           .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd};
        r = dev->import_semaphore_fd(dev->device, &info);
        if (r != VK_SUCCESS) {
            close(fd);
            return fail(r, "vkImportSemaphoreFdKHR failed (%d)", r);
        }
    }
    return VK_SUCCESS;
}

static void close_fds(const te_context_desc* d) {
    for (int i = 0; i < 2; ++i)
        if (d->source_fds[i] >= 0) close(d->source_fds[i]);
    for (int i = 0; i < TE_MAX_OUTPUTS; ++i)
        if (d->dest_fds[i] >= 0) close(d->dest_fds[i]);
    if (d->sync_fd >= 0) close(d->sync_fd);
}

static VkResult validate_desc(te_device* dev, const te_context_desc* d) {
    const te_params* p = &d->params;
    if (d->application_timeline && !dev->borrowed)
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "application timeline requires borrowed device");
    if(p->algorithm>TE_ALGO_SHARED_FLOW) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(p->algorithm==TE_ALGO_SHARED_FLOW && (d->shared_flow_dense<0 || d->shared_flow_dense>5))
        return fail(VK_ERROR_FEATURE_NOT_PRESENT,"unknown experimental shared-flow variant");
    if((p->algorithm==TE_ALGO_LMC_GPU || (p->algorithm==TE_ALGO_SHARED_FLOW && d->shared_flow_dense>=3 && d->shared_flow_dense<=4)) &&
        (dev->props.limits.maxComputeWorkGroupInvocations<512 || dev->props.limits.maxComputeWorkGroupSize[0]<512 ||
         dev->props.limits.maxComputeSharedMemorySize<16900))
        return fail(VK_ERROR_FEATURE_NOT_PRESENT,"GPU LMC requires 512-thread groups and 16900 bytes shared memory");
    if((p->algorithm==TE_ALGO_LMC_GPU || (p->algorithm==TE_ALGO_SHARED_FLOW && d->shared_flow_dense>=3 && d->shared_flow_dense<=4)) && !dev->subgroup_arithmetic)
        return fail(VK_ERROR_FEATURE_NOT_PRESENT,"GPU LMC requires compute subgroup arithmetic");
    if (dev->borrowed && ((p->algorithm != TE_ALGO_TILES && p->algorithm != TE_ALGO_LMC_GPU && p->algorithm != TE_ALGO_SHARED_FLOW) || d->export_fds || d->sync_fd >= 0 ||
            d->source_fds[0] >= 0 || d->source_fds[1] >= 0))
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "borrowed device requires same-queue GPU algorithm without external descriptors");
    if (dev->borrowed)
        for (uint32_t i = 0; i < TE_MAX_OUTPUTS; ++i)
            if (d->dest_fds[i] >= 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (d->color_transfer > TE_TRANSFER_PQ ||
            (d->color_transfer == TE_TRANSFER_PQ && (!d->source_format || !d->output_fp16 ||
             (p->algorithm != TE_ALGO_LMC_GPU && p->algorithm != TE_ALGO_SHARED_FLOW))))
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "PQ reconstruction requires packed10 GPU LMC/GSF and FP16 output");
    if (d->source_format || d->output_fp16) {
        if (!dev->borrowed || d->host_io || d->source_bgra || d->output_bgra ||
                !d->output_fp16 || (d->source_format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                d->source_format != VK_FORMAT_A2R10G10B10_UNORM_PACK32))
            return fail(VK_ERROR_FEATURE_NOT_PRESENT, "HDR requires GPU-only borrowed packed10 input and fp16 output");
        if (!format_ok(dev, d->source_format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ||
                !format_ok(dev, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "GPU lacks native HDR input/output formats");
        if (d->color_transfer == TE_TRANSFER_PQ) {
            if(dev->props.limits.maxPerStageDescriptorSamplers < 18 ||
               dev->props.limits.maxPerStageDescriptorSampledImages < 18)
                return fail(VK_ERROR_FEATURE_NOT_PRESENT, "PQ reconstruction requires 18 sampled images");
            if(!format_ok(dev,VK_FORMAT_R16G16B16A16_SFLOAT,
               VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
                return fail(VK_ERROR_FORMAT_NOT_SUPPORTED,"GPU lacks filterable linear HDR cache");
        }
    }
    if (d->output_bgra && p->algorithm == TE_ALGO_LMC)
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "BGRA output packing requires tile or GPU LMC algorithm");
    if (p->tile != 8 && p->tile != 16 && p->tile != 32) return fail(VK_ERROR_INITIALIZATION_FAILED, "tile must be 8, 16 or 32");
    if (p->levels < 1 || p->levels > TE_MAX_LEVELS) return fail(VK_ERROR_INITIALIZATION_FAILED, "levels must be 1..3");
    if (p->probe < 1 || p->probe > 8) return fail(VK_ERROR_INITIALIZATION_FAILED, "probe must be 1..8");
    if (p->full_density != 1 && p->full_density != 2)
        return fail(VK_ERROR_INITIALIZATION_FAILED, "full_density must be 1 or 2");
    if (p->coarse_range < 1 || p->coarse_range > 8)
        return fail(VK_ERROR_INITIALIZATION_FAILED, "coarse_range must be 1..8");
    if (d->outputs < 1 || d->outputs > TE_MAX_OUTPUTS) return fail(VK_ERROR_INITIALIZATION_FAILED, "outputs must be 1..4");
    if (d->width < 16 || d->height < 16 || d->width > 8192 || d->height > 8192)
        return fail(VK_ERROR_INITIALIZATION_FAILED, "unsupported extent %ux%u", d->width, d->height);
    if (d->width > dev->props.limits.maxImageDimension2D || d->height > dev->props.limits.maxImageDimension2D)
        return fail(VK_ERROR_FORMAT_NOT_SUPPORTED, "extent exceeds the device limit");
    if ((d->width + p->tile - 1) / p->tile * ((d->height + p->tile - 1) / p->tile) > dev->props.limits.maxComputeWorkGroupCount[0])
        return fail(VK_ERROR_INITIALIZATION_FAILED, "tile count exceeds the dispatch limit");
    return VK_SUCCESS;
}

VkResult te_context_create(te_device* dev, const te_context_desc* desc, te_context** out) {
    double context_started = te_profile_init_clock_ms();
    select_device(dev);
    *out = NULL;
    VkResult r = validate_desc(dev, desc);
    if (r != VK_SUCCESS) {
        close_fds(desc);
        return r;
    }
    te_context* ctx = calloc(1, sizeof *ctx);
    if (!ctx) {
        close_fds(desc);
        return fail(VK_ERROR_OUT_OF_HOST_MEMORY, "out of memory");
    }
    ctx->dev = dev;
    ctx->desc = *desc;
    ctx->upload_ring[0] = ctx->upload_ring[1] = -1;
    VkDeviceSize allocated_before = dev->allocated;
    const te_params* p = &desc->params;
    uint32_t W = desc->width, H = desc->height, T = p->tile;
    ctx->tx = (W + T - 1) / T;
    ctx->ty = (H + T - 1) / T;
    ctx->tiles = ctx->tx * ctx->ty;
    ctx->lw[0] = W;
    ctx->lh[0] = H;
    for (int l = 1; l < TE_LUMA_IMAGES; ++l) {
        ctx->lw[l] = ctx->lw[l - 1] / 2 > 0 ? ctx->lw[l - 1] / 2 : 1;
        ctx->lh[l] = ctx->lh[l - 1] / 2 > 0 ? ctx->lh[l - 1] / 2 : 1;
    }
    /* Ownership of every supplied descriptor passes to the context here. */
    int fds_taken[2 + TE_MAX_OUTPUTS] = {0};
    VkDevice d = dev->device;
#define TRY(x) do { r = (x); if (r != VK_SUCCESS) goto error; } while (0)
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = dev->qfi};
    TRY(vkCreateCommandPool(d, &cpci, NULL, &ctx->cmdpool));
    for (int s = 0; s < 2; ++s) {
        fds_taken[s] = 1;
        /* imports use the exporter's transfer/sampled contract (as MAKO's native source path) */
        TRY(image_create(dev, &ctx->src[s], W, H, desc->source_format ? desc->source_format :
                         desc->source_bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, desc->source_fds[s],
                         desc->export_fds && desc->source_fds[s] < 0));
        for (int l = 0; l < TE_LUMA_IMAGES; ++l)
            TRY(image_create(dev, &ctx->luma[s][l], ctx->lw[l], ctx->lh[l], dev->luma_format,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             -1, 0));
    }
    for (uint32_t i = 0; i < TE_MAX_OUTPUTS; ++i) {
        fds_taken[2 + i] = 1;
        if (i >= desc->outputs) {
            if (desc->dest_fds[i] >= 0) close(desc->dest_fds[i]);
            continue;
        }
        /* imported destinations use the same usage as MAKO's own backend import */
        VkImageUsageFlags du = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (desc->dest_fds[i] < 0 || desc->host_io) du |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; /* host_io: test readback */
        TRY(image_create(dev, &ctx->dst[i], W, H, desc->output_fp16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM, du, desc->dest_fds[i],
                         desc->export_fds && desc->dest_fds[i] < 0));
    }
    {
        int fd = desc->sync_fd;
        ctx->desc.sync_fd = -1;
        if (!dev->borrowed) TRY(import_timeline(ctx, fd));
        else ctx->timeline = desc->application_timeline;
    }
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f};
    TRY(vkCreateSampler(d, &sci, NULL, &ctx->sampler));
    VkDeviceSize vec_size = (VkDeviceSize)TE_LUMA_IMAGES * ctx->tiles * 8;
    TRY(buffer_create(dev, &ctx->params, sizeof(te_ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1));
    for (int s = 0; s < 2; ++s)
        TRY(buffer_create(dev, &ctx->vectors[s], vec_size,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0));
    TRY(buffer_create(dev, &ctx->stats, (VkDeviceSize)ctx->tiles * 32,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0));
    TRY(buffer_create(dev, &ctx->states, (VkDeviceSize)ctx->tiles * 16,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0));
    TRY(buffer_create(dev, &ctx->residual, (VkDeviceSize)ctx->tiles * 4,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0));
    TRY(buffer_create(dev, &ctx->globals, TE_GLOBALS_HEADER + (VkDeviceSize)ctx->tiles * 12,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0));
    TRY(buffer_create(dev, &ctx->diag, (VkDeviceSize)ctx->tiles * 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1));
    if (desc->host_io) {
        for (int s = 0; s < 2; ++s)
            TRY(buffer_create(dev, &ctx->upload[s], (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1));
        for (uint32_t i = 0; i < desc->outputs; ++i)
            TRY(buffer_create(dev, &ctx->download[i], (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1));
    }
    te_ubo ubo;
    memset(&ubo, 0, sizeof ubo);
    ubo.size[0] = (int32_t)W;
    ubo.size[1] = (int32_t)H;
    ubo.tiles[0] = (int32_t)ctx->tx;
    ubo.tiles[1] = (int32_t)ctx->ty;
    ubo.static_thr = p->static_thr;
    ubo.static_max_thr = p->static_max_thr;
    ubo.light_thr = p->light_thr;
    ubo.cut_thr = p->cut_thr;
    ubo.residual_thr = p->residual_thr;
    ubo.occ_lo = p->occ_lo;
    ubo.occ_hi = p->occ_hi;
    ubo.smooth_bias = p->smooth_bias;
    ubo.me_bias = p->me_bias;
    ubo.coarse_range = p->coarse_range;
    ubo.hysteresis = (int32_t)p->hysteresis;
    ubo.levels = (int32_t)p->levels;
    ubo.adaptive = p->adaptive;
    ubo.force_class = p->force_class;
    ubo.tile = (int32_t)T;
    ubo.probe = (int32_t)p->probe;
    ubo.full_density = (int32_t)p->full_density;
    ubo.pad[0] = desc->output_bgra;
    for (int l = 0; l < TE_LUMA_IMAGES; ++l) {
        ubo.level_size[l][0] = (float)ctx->lw[l];
        ubo.level_size[l][1] = (float)ctx->lh[l];
    }
    memcpy(ctx->params.map, &ubo, sizeof ubo);
    TRY(create_layouts(ctx));
    VkDescriptorSetLayout l0[2] = {ctx->dsl0, ctx->dsl0};
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = ctx->pool, .descriptorSetCount = 2, .pSetLayouts = l0};
    TRY(vkAllocateDescriptorSets(d, &dsai, ctx->set0));
    VkDescriptorSetLayout l1[TE_MAX_OUTPUTS] = {ctx->dsl1, ctx->dsl1, ctx->dsl1, ctx->dsl1};
    dsai.descriptorSetCount = desc->outputs;
    dsai.pSetLayouts = l1;
    TRY(vkAllocateDescriptorSets(d, &dsai, ctx->set1));
    write_sets(ctx);
    if (p->algorithm == TE_ALGO_LMC || p->algorithm == TE_ALGO_LMC_GPU || p->algorithm == TE_ALGO_SHARED_FLOW) {
        /* The algorithm is fixed for this context. Both analysis and output
         * dispatch exclusively through lmc_record_* and its own pipelines;
         * compiling the nine tile pipelines here wastes initialization work. */
        double lmc_started = te_profile_init_clock_ms();
        TRY(lmc_create(ctx));
        te_profile_init_log("lmc-context-all", -1, p->algorithm, lmc_started, VK_SUCCESS);
    } else {
        double pipeline_started = te_profile_init_clock_ms();
        TRY(create_pipelines(ctx));
        te_profile_init_log("temporal-pipelines-all", -1, 0, pipeline_started, VK_SUCCESS);
    }
    for (int s = 0; s < TE_RING; ++s) {
        te_slot* slot = &ctx->ring[s];
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = ctx->cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
        TRY(vkAllocateCommandBuffers(d, &cai, &slot->analysis));
        TRY(vkAllocateCommandBuffers(d, &cai, &slot->analysis2));
        cai.commandBufferCount = TE_MAX_OUTPUTS;
        TRY(vkAllocateCommandBuffers(d, &cai, slot->out));
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        TRY(vkCreateFence(d, &fci, NULL, &slot->fence));
        VkQueryPoolCreateInfo qpci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                      .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = TE_QUERIES};
        TRY(vkCreateQueryPool(d, &qpci, NULL, &slot->queries));
        TRY(buffer_create(dev, &slot->readback, TE_GLOBALS_HEADER * TE_MAX_OUTPUTS, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1));
    }
    double init_submit_started = te_profile_init_clock_ms();
    TRY(one_time(ctx, record_init, NULL));
    te_profile_init_log("context-initial-gpu-submit-and-wait", -1, 0, init_submit_started, VK_SUCCESS);
    ctx->memory_bytes = dev->allocated - allocated_before;
    te_profile_init_log("context-total", -1, p->algorithm, context_started, VK_SUCCESS);
#undef TRY
    *out = ctx;
    return VK_SUCCESS;
error:
    for (int i = 0; i < 2; ++i)
        if (!fds_taken[i] && desc->source_fds[i] >= 0) close(desc->source_fds[i]);
    for (int i = 0; i < TE_MAX_OUTPUTS; ++i)
        if (!fds_taken[2 + i] && desc->dest_fds[i] >= 0) close(desc->dest_fds[i]);
    if (!ctx->timeline && ctx->desc.sync_fd >= 0) close(ctx->desc.sync_fd);
    {
        char saved[sizeof g_error];
        memcpy(saved, g_error, sizeof saved);
        if (!saved[0]) snprintf(saved, sizeof saved, "context creation failed (%d)", r);
        te_context_destroy(ctx);
        memcpy(g_error, saved, sizeof saved);
    }
    return r;
}

void te_context_destroy(te_context* ctx) {
    if (!ctx) return;
    select_device(ctx->dev);
    te_device* dev = ctx->dev;
    VkDevice d = dev->device;
    (void)te_resume_outputs(ctx);
    if (ctx->lmc) lmc_drain(ctx);   /* the worker's pending submissions first */
    if (dev->borrowed) {
        /* Only our fences: never stop the application's other queues. */
        for (int i = 0; i < TE_RING; ++i)
            if (ctx->ring[i].in_flight) {
                if(ctx->ring[i].deferred_failed)
                    (void)te_timeline_wait(ctx,ctx->ring[i].deferred_first,UINT64_MAX);
                else vkWaitForFences(d, 1, &ctx->ring[i].fence, VK_TRUE, UINT64_MAX);
            }
    } else {
        pthread_mutex_lock(&dev->queue_lock);
        vkDeviceWaitIdle(d);
        pthread_mutex_unlock(&dev->queue_lock);
    }
    lmc_destroy(ctx);
    for (int s = 0; s < TE_RING; ++s) {
        te_slot* slot = &ctx->ring[s];
        if (slot->fence) vkDestroyFence(d, slot->fence, NULL);
        if (slot->queries) vkDestroyQueryPool(d, slot->queries, NULL);
        buffer_destroy(dev, &slot->readback);
    }
    for (int i = 0; i < PIPE_COUNT; ++i)
        if (ctx->pipe[i]) vkDestroyPipeline(d, ctx->pipe[i], NULL);
    if (ctx->pool) vkDestroyDescriptorPool(d, ctx->pool, NULL);
    if (ctx->layout) vkDestroyPipelineLayout(d, ctx->layout, NULL);
    if (ctx->dsl0) vkDestroyDescriptorSetLayout(d, ctx->dsl0, NULL);
    if (ctx->dsl1) vkDestroyDescriptorSetLayout(d, ctx->dsl1, NULL);
    if (ctx->sampler) vkDestroySampler(d, ctx->sampler, NULL);
    if (ctx->timeline && !ctx->desc.application_timeline) vkDestroySemaphore(d, ctx->timeline, NULL);
    buffer_destroy(dev, &ctx->params);
    for (int s = 0; s < 2; ++s) {
        buffer_destroy(dev, &ctx->vectors[s]);
        buffer_destroy(dev, &ctx->upload[s]);
        image_destroy(dev, &ctx->src[s]);
        for (int l = 0; l < TE_LUMA_IMAGES; ++l) image_destroy(dev, &ctx->luma[s][l]);
    }
    for (int i = 0; i < TE_MAX_OUTPUTS; ++i) {
        image_destroy(dev, &ctx->dst[i]);
        buffer_destroy(dev, &ctx->download[i]);
    }
    buffer_destroy(dev, &ctx->stats);
    buffer_destroy(dev, &ctx->states);
    buffer_destroy(dev, &ctx->residual);
    buffer_destroy(dev, &ctx->globals);
    buffer_destroy(dev, &ctx->diag);
    if (ctx->cmdpool) vkDestroyCommandPool(d, ctx->cmdpool, NULL);
    free(ctx);
}

/* ------------------------------------------------------------------ recording */

static void push_knobs(te_context* ctx, VkCommandBuffer cb, float t, int level, int frame, int parity, float scale,
                       int flags) {
    te_push pc = {.t = t, .level = level, .frame = frame, .parity = parity, .step = scale, .flags = flags};
    vkCmdPushConstants(cb, ctx->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
}

static void push(te_context* ctx, VkCommandBuffer cb, float t, int level, int frame, int parity) {
    te_push pc = {.t = t, .level = level, .frame = frame, .parity = parity, .step = 1.0f};
    vkCmdPushConstants(cb, ctx->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
}

static uint32_t groups(uint32_t n, uint32_t g) { return (n + g - 1) / g; }

#include "te_lmc.inc"

static void record_analysis(te_context* ctx, te_slot* slot, const te_submit* s, int generate) {
    VkCommandBuffer cb = slot->analysis;
    uint32_t par = s->slot;
    const te_params* p = &ctx->desc.params;
    vkCmdResetQueryPool(cb, slot->queries, 0, TE_QUERIES);
    if (ctx->upload_pending[par]) {
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {ctx->desc.width, ctx->desc.height, 1}};
        vkCmdCopyBufferToImage(cb, ctx->upload[par].buf, ctx->src[par].img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    }
    ownership(cb, &ctx->src[par], ctx->dev->qfi, 1);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot->queries, 0);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->layout, 0, 1, &ctx->set0[par], 0, NULL);
    compute_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipe[PIPE_LUMA]);
    push(ctx, cb, 0.0f, 0, 0, (int)par);
    vkCmdDispatch(cb, groups(ctx->lw[0], 8), groups(ctx->lh[0], 8), 1);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipe[PIPE_DOWN]);
    for (uint32_t l = 0; l < p->levels; ++l) {
        compute_barrier(cb);
        push(ctx, cb, 0.0f, (int)l, 0, (int)par);
        vkCmdDispatch(cb, groups(ctx->lw[l + 1], 8), groups(ctx->lh[l + 1], 8), 1);
    }
    compute_barrier(cb);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 1);
    if (!generate) {
        /* history only: the next pair must not trust a temporal predictor.
         * Outputs of a hold (first frame or discontinuity) copy the new frame. */
        vkCmdFillBuffer(cb, ctx->globals.buf, 16 + 4 * par, 4, 0);
        vkCmdFillBuffer(cb, ctx->globals.buf, 24 + 4 * par, 4, 1);
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 3);
        return;
    }
    vkCmdFillBuffer(cb, ctx->globals.buf, 4 * par, 4, 0);
    vkCmdFillBuffer(cb, ctx->globals.buf, 8 + 4 * par, 4, 0);
    vkCmdFillBuffer(cb, ctx->globals.buf, 32 + 4 * par, 4, 0);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipe[PIPE_ME]);
    for (int l = (int)p->levels; l >= 0; --l) {
        push(ctx, cb, 0.5f, l, 0, (int)par);
        vkCmdDispatch(cb, ctx->tiles, 1, 1); /* one workgroup per tile */
        compute_barrier(cb);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipe[PIPE_STATS]);
    push(ctx, cb, 0.5f, 0, 0, (int)par);
    vkCmdDispatch(cb, ctx->tx, ctx->ty, 1);
    compute_barrier(cb);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 2);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipe[PIPE_CLASSIFY]);
    push_knobs(ctx, cb, 0.5f, 0, 0, (int)par, s->light_scale > 0.0f ? s->light_scale : 1.0f,
               (s->force_full ? 1 : 0) | (s->force_light ? 2 : 0));
    vkCmdDispatch(cb, groups(ctx->tiles, 64), 1, 1);
    compute_barrier(cb);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 3);
}

static void record_output(te_context* ctx, te_slot* slot, const te_submit* s, uint32_t i, int hold) {
    VkCommandBuffer cb = slot->out[i];
    uint32_t par = s->slot;
    uint32_t q = 4 + i * 6;
    int profile = ctx->desc.profile || s->profile;
    ownership(cb, &ctx->dst[i], ctx->dev->qfi, 1);
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const uint32_t args[12] = {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0};
    vkCmdUpdateBuffer(cb, ctx->globals.buf, 48, sizeof args, args);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, q + 0);
    VkDescriptorSet sets[2] = {ctx->set0[par], ctx->set1[i]};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->layout, 0, 2, sets, 0, NULL);
    float t = hold ? 1.0f : s->timestamps[i]; /* a hold shows the newest real frame */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipe[PIPE_LISTS]);
    push_knobs(ctx, cb, t, 0, (int)i, (int)par, 1.0f, s->force_light ? 2 : 0);
    vkCmdDispatch(cb, groups(ctx->tiles, 64), 1, 1);
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, q + 1);
    const int pipes[3] = {PIPE_REUSE, PIPE_LIGHT, PIPE_FULL};
    for (int c = 0; c < 3; ++c) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipe[pipes[c]]);
        vkCmdDispatchIndirect(cb, ctx->globals.buf, 48 + 16 * (VkDeviceSize)c);
        if (profile) compute_barrier(cb);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, q + 2 + (uint32_t)c);
    }
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, q + 5);
    if (s->download && ctx->desc.host_io) {
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {ctx->desc.width, ctx->desc.height, 1}};
        vkCmdCopyImageToBuffer(cb, ctx->dst[i].img, VK_IMAGE_LAYOUT_GENERAL, ctx->download[i].buf, 1, &region);
    }
    VkBufferCopy copy = {0, (VkDeviceSize)TE_GLOBALS_HEADER * i, TE_GLOBALS_HEADER};
    vkCmdCopyBuffer(cb, ctx->globals.buf, slot->readback.buf, 1, &copy);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
    ownership(cb, &ctx->dst[i], ctx->dev->qfi, 0);
}

static void collect(te_context* ctx, te_slot* slot) {
    if (!slot->generated) return;
    te_pair_stats* st = &ctx->last;
    memset(st, 0, sizeof *st);
    st->pair = slot->pair;
    st->outputs = slot->outputs;
    st->tile_total = ctx->tiles;
    st->profiled = slot->profiled;
    st->held = slot->held;
    st->sequence = slot->sequence;
    ctx->last_timing_sequence=slot->sequence;
    ctx->last_submit_return_ns=slot->submit_return_ns;
    ctx->last_gpu_timestamp_period_ns=ctx->dev->props.limits.timestampPeriod;
    ctx->last_gpu_timestamp_valid_bits=ctx->dev->ts_bits;
    ctx->last_gpu_start_ticks=0;
    ctx->outputs_completed += slot->outputs;
    st->outputs_completed_total = ctx->outputs_completed;
    uint64_t ts[TE_QUERIES];
    uint32_t used = 4 + slot->outputs * 6;
    st->timed = ctx->dev->ts_bits > 0 &&
                vkGetQueryPoolResults(ctx->dev->device, slot->queries, 0, used, sizeof ts, ts, sizeof ts[0],
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    double period = ctx->dev->props.limits.timestampPeriod * 1e-6;
    uint64_t mask = ctx->dev->ts_bits >= 64 ? ~0ull : ((1ull << ctx->dev->ts_bits) - 1);
#define DELTA(a, b) ((double)((ts[b] - ts[a]) & mask) * period)
    if (st->timed) {
        ctx->last_gpu_start_ticks = ts[0];
        st->stage_ms[TE_STAGE_PYRAMID] = DELTA(0, 1);
        st->stage_ms[TE_STAGE_ANALYZE] = DELTA(1, 2);
        st->stage_ms[TE_STAGE_CLASSIFY] = DELTA(2, 3);
        for (uint32_t i = 0; i < slot->outputs; ++i) {
            uint32_t q = 4 + i * 6;
            st->ready_ms[i] = DELTA(0, q + 5);
            st->frame_ms[i][TE_FRAME_LISTS] = DELTA(q, q + 1);
            if (slot->profiled) {
                st->frame_ms[i][TE_FRAME_REUSE] = DELTA(q + 1, q + 2);
                st->frame_ms[i][TE_FRAME_LIGHT] = DELTA(q + 2, q + 3);
                st->frame_ms[i][TE_FRAME_FULL] = DELTA(q + 3, q + 4);
            } else {
                st->frame_ms[i][TE_FRAME_REUSE] = st->frame_ms[i][TE_FRAME_LIGHT] = st->frame_ms[i][TE_FRAME_FULL] = -1.0;
            }
            st->frame_ms[i][TE_FRAME_TOTAL] = DELTA(q, q + 5);
        }
    }
    for (int k = 0; k < TE_LMC_PROF_COUNT; ++k) st->lmc_ms[k] = -1.0;
    if (st->timed && slot->lmc_timed &&
        vkGetQueryPoolResults(ctx->dev->device, slot->queries, TE_LMC_Q0, TE_LMC_PROF_COUNT + 1,
                              sizeof ts, ts + TE_LMC_Q0, sizeof ts[0],
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        for (int k = 0; k < TE_LMC_PROF_COUNT; ++k) st->lmc_ms[k] = DELTA(TE_LMC_Q0 + k, TE_LMC_Q0 + k + 1);
    /* the wait for the CPU motion fit sits between stage points 2 and 3: GPU time is without it */
    st->shared_flow=ctx->lmc && ctx->lmc->shared_flow;
    st->model_gpu = ctx->lmc && ctx->lmc->gpu_fit && !ctx->lmc->shared_flow;
    if (ctx->lmc && !ctx->lmc->gpu_fit && st->lmc_ms[TE_LMC_FIT_WAIT] > 0.0) {
        st->stage_ms[TE_STAGE_CLASSIFY] -= st->lmc_ms[TE_LMC_FIT_WAIT];
        if (st->stage_ms[TE_STAGE_CLASSIFY] < 0.0) st->stage_ms[TE_STAGE_CLASSIFY] = 0.0;
    }
#undef DELTA
    const uint8_t* rb = slot->readback.map;
    if (ctx->lmc) {
        st->lmc_fit_ms = slot->lmc_fit_ms;
        st->lmc_tier = slot->lmc_tier;
        /* LMC: tiles[i] = layer count, orphan blocks (B grid, A grid) */
        for (uint32_t i = 0; i < slot->outputs; ++i) {
            const uint32_t* g = (const uint32_t*)(rb + TE_GLOBALS_HEADER * i);
            for (int c = 0; c < 3; ++c) st->tiles[i][c] = g[c];
        }
        if(ctx->lmc->shared_flow){st->scene_cut=((const uint32_t*)rb)[3];st->shared_flow_fallback=((const uint32_t*)rb)[4];
            st->shared_flow_cut_bad_blocks=((const uint32_t*)rb)[5];st->source_duplicate=((const uint32_t*)rb)[6]==0;}
        st->profiled = 0;
        ctx->have_last = 1;
        slot->generated = 0;
        return;
    }
    for (uint32_t i = 0; i < slot->outputs; ++i) {
        const uint32_t* g = (const uint32_t*)(rb + TE_GLOBALS_HEADER * i);
        for (int c = 0; c < 3; ++c) st->tiles[i][c] = g[12 + 4 * c];
    }
    const uint32_t* g = (const uint32_t*)(rb + TE_GLOBALS_HEADER * (slot->outputs - 1));
    st->scene_cut = g[6 + slot->parity];
    st->promoted = g[10];
    st->confidence = ctx->tiles ? (double)g[8 + slot->parity] / 1000.0 / ctx->tiles : 0.0;
    ctx->have_last = 1;
    slot->generated = 0;
}

static VkResult finish_deferred_output(te_context* ctx,int produce) {
    if(!ctx)return VK_ERROR_INITIALIZATION_FAILED;
    te_slot *slot=ctx->deferred_slot;
    if(!slot)return VK_SUCCESS;
    select_device(ctx->dev);
    ctx->deferred_slot=NULL;
    VkTimelineSemaphoreSubmitInfo timeline={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&slot->deferred_second};
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&timeline,
        .commandBufferCount=produce?1u:0u,.pCommandBuffers=produce?&slot->out[1]:NULL,
        .signalSemaphoreCount=1,.pSignalSemaphores=&ctx->timeline};
    if(!produce){slot->outputs=1;ctx->output_ready[1]=0;}
    VkResult result=queue_submit(ctx->dev,1,&submit,slot->fence);
    if(result!=VK_SUCCESS) {
        /* No fence was submitted for G2. Cleanup waits only the known G1
         * signal, not the impossible final signal/fence. */
        slot->deferred_failed=1;slot->async_result=result;
        return fail(result,"deferred G2 submission failed (%d)",result);
    }
    return VK_SUCCESS;
}
VkResult te_resume_outputs(te_context* ctx) { return finish_deferred_output(ctx,1); }
VkResult te_cancel_deferred_output(te_context* ctx) { return finish_deferred_output(ctx,0); }

static VkResult wait_slot(te_context* ctx, te_slot* slot, int nonblocking) {
    select_device(ctx->dev);
    if (!slot->in_flight) return VK_SUCCESS;
    if(ctx->deferred_slot==slot) {
        if(nonblocking)return VK_NOT_READY;
        (void)te_resume_outputs(ctx);
    }
    if (ctx->lmc && lmc_slot_pending(ctx, slot)) {   /* the fence is the worker's until it submits */
        if (nonblocking) return VK_NOT_READY;
        lmc_drain(ctx);
    }
    VkDevice d = ctx->dev->device;
    VkResult r=slot->deferred_failed?te_timeline_wait(ctx,slot->deferred_first,nonblocking?0:UINT64_MAX):
        nonblocking?vkGetFenceStatus(d,slot->fence):vkWaitForFences(d,1,&slot->fence,VK_TRUE,UINT64_MAX);
    if (r == VK_NOT_READY || r == VK_TIMEOUT) return VK_NOT_READY;
    if (r != VK_SUCCESS) return fail(r, "fence wait failed (%d)", r);
    if (slot->async_result != VK_SUCCESS) {
        VkResult failed = slot->async_result;
        slot->generated = 0;
        slot->in_flight = 0;
        vkResetFences(d, 1, &slot->fence);
        return fail(failed, "output submission failed (%d)", failed);
    }
    collect(ctx, slot);
    vkResetFences(d, 1, &slot->fence);
    slot->in_flight = 0;
    return VK_SUCCESS;
}

VkResult te_upload_rgb(te_context* ctx, uint32_t slot, const uint8_t* rgb) {
    if (!ctx->desc.host_io || slot > 1) return fail(VK_ERROR_INITIALIZATION_FAILED, "upload needs a host_io context");
    if (ctx->upload_ring[slot] >= 0) {
        VkResult r = wait_slot(ctx, &ctx->ring[ctx->upload_ring[slot]], 0);
        if (r != VK_SUCCESS) return r;
    }
    uint8_t* dst = ctx->upload[slot].map;
    size_t n = (size_t)ctx->desc.width * ctx->desc.height;
    for (size_t i = 0; i < n; ++i) {
        dst[4 * i + 0] = rgb[3 * i + 0];
        dst[4 * i + 1] = rgb[3 * i + 1];
        dst[4 * i + 2] = rgb[3 * i + 2];
        dst[4 * i + 3] = 255;
    }
    ctx->upload_pending[slot] = 1;
    return VK_SUCCESS;
}

VkResult te_submit_frame(te_context* ctx, const te_submit* s) {
    select_device(ctx->dev);
    if (ctx->dev->borrowed && !ctx->timeline && (s->wait_value || s->signal_first))
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "borrowed queue uses submission order, not timeline values");
    if (s->slot > 1) return fail(VK_ERROR_INITIALIZATION_FAILED, "slot must be 0 or 1");
    int repeat = ctx->frames > 0 && s->slot == ctx->last_slot;
    if (repeat && !s->discontinuity)
        return fail(VK_ERROR_INITIALIZATION_FAILED, "source slots must alternate");
    if (s->count > ctx->desc.outputs) return fail(VK_ERROR_INITIALIZATION_FAILED, "more timestamps than outputs");
    if(s->defer_second && (!ctx->dev->borrowed || !ctx->timeline || !s->signal_first || s->count!=2 ||
       !ctx->lmc || !ctx->lmc->shared_flow))
        return fail(VK_ERROR_FEATURE_NOT_PRESENT,"deferred G2 requires borrowed timeline GSF with two outputs");
    for (uint32_t i = 0; i < s->count; ++i) {
        float t = s->timestamps[i];
        if (!(t > 0.0f && t < 1.0f) || (i && !(t > s->timestamps[i - 1])))
            return fail(VK_ERROR_INITIALIZATION_FAILED, "timestamps must increase strictly inside (0, 1)");
    }
    VkResult previous=te_resume_outputs(ctx);
    if(previous!=VK_SUCCESS)return previous;
    /* LMC: the previous pair's second half is submitted (by the fit worker) before anything else */
    if (ctx->lmc) {
        if (s->nonblocking) {
            pthread_mutex_lock(&ctx->lmc->fit_lock);
            int busy = ctx->lmc->job_count || ctx->lmc->fit_busy;
            pthread_mutex_unlock(&ctx->lmc->fit_lock);
            if (busy) return VK_NOT_READY;
        } else lmc_drain(ctx);
    }
    /* No previous frame (first frame, or a skipped one): outputs hold the new frame */
    int generate = ctx->frames > 0 && s->count > 0 && !repeat && !s->discontinuity && !s->hold;
    uint32_t r_index = ctx->ring_next;
    te_slot* slot = &ctx->ring[r_index];
    VkResult r = wait_slot(ctx, slot, s->nonblocking);
    if (r != VK_SUCCESS) return r;
    slot->lmc_fit_ms = 0.0;
    slot->async_result = VK_SUCCESS;
    slot->deferred_failed=0;slot->deferred_first=slot->deferred_second=0;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(slot->analysis, 0);
    vkBeginCommandBuffer(slot->analysis, &bi);
    if (ctx->lmc) lmc_record_analysis(ctx, slot, s, generate);
    else record_analysis(ctx, slot, s, generate);
    CHECK(vkEndCommandBuffer(slot->analysis), "vkEndCommandBuffer(analysis)");
    uint32_t outs = s->count; /* every requested output is produced and signalled */
    for (uint32_t i = 0; i < outs; ++i) {
        vkResetCommandBuffer(slot->out[i], 0);
        vkBeginCommandBuffer(slot->out[i], &bi);
        if (ctx->lmc) lmc_record_output(ctx, slot, s, i, !generate);
        else record_output(ctx, slot, s, i, !generate);
        CHECK(vkEndCommandBuffer(slot->out[i]), "vkEndCommandBuffer(output)");
    }
    /* analysis, then the outputs.  LMC generated pairs: the analysis' first half is submitted here; the
       fit worker submits the second half and the outputs once the motion model of the pair is in */
    int split = ctx->lmc && slot->lmc_split;
    VkSubmitInfo si[1 + TE_MAX_OUTPUTS];
    VkTimelineSemaphoreSubmitInfo tsi[1 + TE_MAX_OUTPUTS];
    uint64_t wait_values[1] = {s->wait_value};
    uint64_t signal_values[TE_MAX_OUTPUTS];
    static const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    uint64_t fit_ready = split ? 2 * slot->lmc_seq + 1 : 0;
    si[0] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                           .pCommandBuffers = &slot->analysis};
    tsi[0] = (VkTimelineSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    if (s->wait_value) {
        tsi[0].waitSemaphoreValueCount = 1;
        tsi[0].pWaitSemaphoreValues = wait_values;
        si[0].waitSemaphoreCount = 1;
        si[0].pWaitSemaphores = &ctx->timeline;
        si[0].pWaitDstStageMask = &wait_stage;
    }
    if (split) {   /* the worker waits for this before it reads the vectors */
        tsi[0].signalSemaphoreValueCount = 1;
        tsi[0].pSignalSemaphoreValues = &fit_ready;
        si[0].signalSemaphoreCount = 1;
        si[0].pSignalSemaphores = &ctx->lmc->fit_tl;
    }
    /* A history-only update has no output submissions. Still publish its
     * completion, so external source copies cannot overwrite an image while
     * the pyramid reads it. Never make the caller wait for an unsent value. */
    uint64_t history_ready = s->signal_first;
    if (!outs && history_ready) {
        tsi[0].signalSemaphoreValueCount = 1;
        tsi[0].pSignalSemaphoreValues = &history_ready;
        si[0].signalSemaphoreCount = 1;
        si[0].pSignalSemaphores = &ctx->timeline;
    }
    if (s->wait_value || split || (!outs && history_ready)) si[0].pNext = &tsi[0];
    /* the outputs (after the second half), into the slot when the worker submits them */
    VkSubmitInfo* os = split ? slot->lsub : si;
    VkTimelineSemaphoreSubmitInfo* ot = split ? slot->ltsi : tsi;
    uint64_t* ov = split ? slot->lsig : signal_values;
    uint32_t n = 1;
    if (split)
        os[0] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                               .pCommandBuffers = &slot->analysis2};
    for (uint32_t i = 0; i < outs; ++i, ++n) {
        os[n] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                               .pCommandBuffers = &slot->out[i]};
        if (s->signal_first) {
            ov[i] = s->signal_first + i;
            ot[n] = (VkTimelineSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                    .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &ov[i]};
            os[n].pNext = &ot[n];
            os[n].signalSemaphoreCount = 1;
            os[n].pSignalSemaphores = &ctx->timeline;
        }
    }
    VkResult sr;
    if (split) {
        slot->lnsub = n;
        sr = queue_submit(ctx->dev, 1, si, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS) return fail(sr, "vkQueueSubmit(analysis) failed (%d)", sr);
        lmc_fit_enqueue(ctx, slot);
    } else {
        sr = queue_submit(ctx->dev,s->defer_second?2:n,si,s->defer_second?VK_NULL_HANDLE:slot->fence);
        if (sr != VK_SUCCESS) return fail(sr, "vkQueueSubmit failed (%d)", sr);
        if(s->defer_second) {
            slot->deferred_first=s->signal_first;slot->deferred_second=s->signal_first+1;
            ctx->deferred_slot=slot;
        }
    }
    struct timespec te_submitted_at;clock_gettime(CLOCK_MONOTONIC,&te_submitted_at);
    const uint64_t te_submit_return_ns=(uint64_t)te_submitted_at.tv_sec*1000000000ull+(uint64_t)te_submitted_at.tv_nsec;
    for(uint32_t i=0;i<TE_MAX_OUTPUTS;++i)ctx->output_ready[i]=i<outs?s->signal_first+i:0;
    slot->in_flight = 1;
    slot->outputs = outs;
    slot->generated = outs > 0;
    slot->held = !generate;
    slot->pair = ctx->pairs;
    slot->parity = s->slot;
    slot->sequence = ++ctx->submits;
    slot->submit_return_ns=te_submit_return_ns;
    slot->profiled = ctx->desc.profile || s->profile;
    if (ctx->upload_pending[s->slot]) {
        ctx->upload_pending[s->slot] = 0;
        ctx->upload_ring[s->slot] = (int)r_index;
    }
    if (generate) ctx->pairs++;
    ctx->frames++;
    ctx->last_slot = s->slot;
    ctx->ring_next = (r_index + 1) % TE_RING;
    return VK_SUCCESS;
}

VkResult te_wait_idle(te_context* ctx) {
    /* oldest first, so the newest pair's statistics are the ones kept */
    for (uint32_t k = 0; k < TE_RING; ++k) {
        VkResult r = wait_slot(ctx, &ctx->ring[(ctx->ring_next + k) % TE_RING], 0);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

int te_context_idle(te_context* ctx, uint64_t timeout_ns) {
    select_device(ctx->dev);
    VkDevice d = ctx->dev->device;
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    if(ctx->deferred_slot) {
        if(!timeout_ns)return 0;
        (void)te_resume_outputs(ctx);
    }
    for (uint32_t k = 0; k < TE_RING; ++k) {
        te_slot* slot = &ctx->ring[(ctx->ring_next + k) % TE_RING];
        if (!slot->in_flight) continue;
        uint64_t remaining = timeout_ns;
        if (timeout_ns != UINT64_MAX) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed = (uint64_t)(now.tv_sec - started.tv_sec) * 1000000000ull
                + (uint64_t)(now.tv_nsec - started.tv_nsec);
            remaining = elapsed < timeout_ns ? timeout_ns - elapsed : 0;
        }
        if (ctx->lmc && !lmc_wait_pending(ctx, slot, remaining)) return 0;
        if (timeout_ns != UINT64_MAX) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed = (uint64_t)(now.tv_sec - started.tv_sec) * 1000000000ull
                + (uint64_t)(now.tv_nsec - started.tv_nsec);
            remaining = elapsed < timeout_ns ? timeout_ns - elapsed : 0;
        }
        VkResult r=slot->deferred_failed?te_timeline_wait(ctx,slot->deferred_first,remaining):
            vkWaitForFences(d,1,&slot->fence,VK_TRUE,remaining);
        if (r != VK_SUCCESS) return 0;
        if (wait_slot(ctx, slot, 0) != VK_SUCCESS) return 0;
    }
    return 1;
}

void te_poll(te_context* ctx) {
    for (uint32_t k = 0; k < TE_RING; ++k) {
        te_slot* slot = &ctx->ring[(ctx->ring_next + k) % TE_RING];
        if (slot->in_flight && wait_slot(ctx, slot, 1) != VK_SUCCESS) break;
    }
}

uint64_t te_context_memory_bytes(const te_context* ctx) { return ctx->memory_bytes; }

uint64_t te_timeline_value(te_context* ctx) {
    select_device(ctx->dev);
    if (!ctx->timeline) return UINT64_MAX;
    uint64_t v = 0;
    if (vkGetSemaphoreCounterValue(ctx->dev->device, ctx->timeline, &v) != VK_SUCCESS) return UINT64_MAX;
    return v;
}

VkResult te_timeline_signal(te_context* ctx, uint64_t value) {
    select_device(ctx->dev);
    if (!ctx->timeline) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkSemaphoreSignalInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = ctx->timeline,
                                  .value = value};
    return vkSignalSemaphore(ctx->dev->device, &info);
}

VkResult te_timeline_wait(te_context* ctx, uint64_t value, uint64_t timeout_ns) {
    select_device(ctx->dev);
    if (!ctx->timeline) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkSemaphoreWaitInfo info = {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount=1,.pSemaphores=&ctx->timeline,.pValues=&value};
    return vkWaitSemaphores(ctx->dev->device,&info,timeout_ns);
}

VkResult te_timeline_fd_probe(te_context* ctx, uint64_t wait_value, uint64_t signal_value) {
    if (!ctx || !ctx->timeline || ctx->dev->borrowed || !wait_value ||
        signal_value <= wait_value)
        return fail(VK_ERROR_FEATURE_NOT_PRESENT, "shared timeline probe needs an imported timeline and increasing values");
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const uint64_t wait = wait_value, signal = signal_value;
    VkTimelineSemaphoreSubmitInfo timeline = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &wait,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signal};
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &ctx->timeline,
        .pWaitDstStageMask = &stage, .signalSemaphoreCount = 1,
        .pSignalSemaphores = &ctx->timeline};
    return queue_submit(ctx->dev, 1, &submit, VK_NULL_HANDLE);
}

uint32_t te_context_outputs(const te_context* ctx) { return ctx->desc.outputs; }

VkImage te_source_image(te_context* ctx, uint32_t slot) {
    return ctx && ctx->dev->borrowed && slot < 2 ? ctx->src[slot].img : VK_NULL_HANDLE;
}
VkImage te_output_image(te_context* ctx, uint32_t output) {
    return ctx && ctx->dev->borrowed && output < ctx->desc.outputs ? ctx->dst[output].img : VK_NULL_HANDLE;
}
void te_device_timestamp_info(const te_device* dev, uint32_t* valid_bits, float* period_ns) {
    if(valid_bits) *valid_bits=dev->ts_bits;
    if(period_ns) *period_ns=dev->props.limits.timestampPeriod;
}

VkResult te_output_control(te_context* ctx,uint32_t output,uint64_t ready_value,
                         uint32_t* scene_cut,int* held,int* duplicate) {
    if(!ctx || !ctx->lmc || !ctx->lmc->shared_flow || !ctx->timeline ||
       output>=TE_MAX_OUTPUTS || !ready_value || ctx->output_ready[output]!=ready_value)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    select_device(ctx->dev);
    uint64_t reached=0;
    VkResult r=vkGetSemaphoreCounterValue(ctx->dev->device,ctx->timeline,&reached);
    if(r!=VK_SUCCESS)return r;
    if(reached<ready_value)return VK_NOT_READY;
    te_slot* slot=&ctx->ring[(ctx->ring_next+TE_RING-1)%TE_RING];
    const uint32_t* header=(const uint32_t*)((const uint8_t*)slot->readback.map+TE_GLOBALS_HEADER*output);
    if(scene_cut)*scene_cut=header[3];
    if(held)*held=slot->held;
    if(duplicate)*duplicate=header[6]==0;
    return VK_SUCCESS;
}

VkResult te_last_stats(te_context* ctx, te_pair_stats* stats) {
    if (!ctx->have_last) return VK_NOT_READY;
    *stats = ctx->last;
    return VK_SUCCESS;
}

VkResult te_last_calibration_fields(te_context* ctx,uint64_t *sequence,
    uint64_t *submit_return_ns,uint64_t *gpu_start_ticks,
    double *gpu_timestamp_period_ns,uint32_t *gpu_timestamp_valid_bits) {
    if (!ctx || !ctx->have_last) return VK_NOT_READY;
    if (!sequence || !submit_return_ns || !gpu_start_ticks ||
        !gpu_timestamp_period_ns || !gpu_timestamp_valid_bits)
        return VK_ERROR_INITIALIZATION_FAILED;
    *sequence=ctx->last_timing_sequence;
    *submit_return_ns=ctx->last_submit_return_ns;
    *gpu_start_ticks=ctx->last_gpu_start_ticks;
    *gpu_timestamp_period_ns=ctx->last_gpu_timestamp_period_ns;
    *gpu_timestamp_valid_bits=ctx->last_gpu_timestamp_valid_bits;
    return VK_SUCCESS;
}

VkResult te_download_rgb(te_context* ctx, uint32_t output, uint8_t* rgb) {
    if (!ctx->desc.host_io || output >= ctx->desc.outputs)
        return fail(VK_ERROR_INITIALIZATION_FAILED, "download needs a host_io context");
    const uint8_t* src = ctx->download[output].map;
    size_t n = (size_t)ctx->desc.width * ctx->desc.height;
    for (size_t i = 0; i < n; ++i) {
        rgb[3 * i + 0] = src[4 * i + (ctx->desc.output_bgra ? 2 : 0)];
        rgb[3 * i + 1] = src[4 * i + 1];
        rgb[3 * i + 2] = src[4 * i + (ctx->desc.output_bgra ? 0 : 2)];
    }
    return VK_SUCCESS;
}

uint32_t te_tile_count(const te_context* ctx, uint32_t* tx, uint32_t* ty) {
    if (tx) *tx = ctx->tx;
    if (ty) *ty = ctx->ty;
    return ctx->tiles;
}

typedef struct { VkBuffer src; VkDeviceSize size; } copy_job;

static void record_copy(te_context* ctx, VkCommandBuffer cb, void* user) {
    copy_job* job = user;
    barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy c = {0, 0, job->size};
    vkCmdCopyBuffer(cb, job->src, ctx->diag.buf, 1, &c);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
}

static VkResult read_buffer(te_context* ctx, VkBuffer src, VkDeviceSize size) {
    VkResult r = te_wait_idle(ctx);
    if (r != VK_SUCCESS) return r;
    copy_job job = {src, size};
    return one_time(ctx, record_copy, &job);
}

VkResult te_read_vectors(te_context* ctx, float* xy) {
    VkResult r = read_buffer(ctx, ctx->vectors[ctx->last_slot].buf, (VkDeviceSize)ctx->tiles * 8);
    if (r == VK_SUCCESS) memcpy(xy, ctx->diag.map, (size_t)ctx->tiles * 8);
    return r;
}

VkResult te_read_classes(te_context* ctx, uint8_t* cls) {
    VkResult r = read_buffer(ctx, ctx->states.buf, (VkDeviceSize)ctx->tiles * 16);
    if (r != VK_SUCCESS) return r;
    const uint32_t* s = ctx->diag.map;
    for (uint32_t i = 0; i < ctx->tiles; ++i) cls[i] = (uint8_t)s[4 * i + 2];
    return VK_SUCCESS;
}

VkResult te_read_stats(te_context* ctx, float* stats8) {
    VkResult r = read_buffer(ctx, ctx->stats.buf, (VkDeviceSize)ctx->tiles * 32);
    if (r == VK_SUCCESS) memcpy(stats8, ctx->diag.map, (size_t)ctx->tiles * 32);
    return r;
}

static int export_memory(te_context* ctx, te_image* im) {
    if (!im->external || im->imported || !ctx->dev->get_memory_fd) return -1;
    VkMemoryGetFdInfoKHR info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = im->mem,
                                 .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd = -1;
    if (ctx->dev->get_memory_fd(ctx->dev->device, &info, &fd) != VK_SUCCESS) return -1;
    return fd;
}

int te_export_source_fd(te_context* ctx, uint32_t slot) { return slot < 2 ? export_memory(ctx, &ctx->src[slot]) : -1; }

int te_export_dest_fd(te_context* ctx, uint32_t output) {
    return output < ctx->desc.outputs ? export_memory(ctx, &ctx->dst[output]) : -1;
}

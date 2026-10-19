/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real XCB WSI smoke for the GPU bridge (GFG_NATIVE_GPU_BRIDGE=1).
 * The app renders N frames of a moving pattern (staging copy, the app's own
 * CPU work) and presents them on FIFO. With GFG_TEST_EXPECT_GPU=1 it reads
 * the layer's process-wide counters (exported test hook) and asserts:
 *   real == N, synthetic == 2*(N-1) - skipped, skipped == 0,
 *   no host pixel reads and no CPU waits on the layer's present path,
 *   GFG_NATIVE_GPU_BRIDGE_VERIFY checks all distinct.
 * With GFG_TEST_VALIDATION=1 it enables VK_LAYER_KHRONOS_validation messages
 * (+ synchronization validation) and fails on any error message.
 * This does not claim scanout, pacing or any hardware behaviour.
 */
#define _GNU_SOURCE
#include <sys/resource.h>
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include <xcb/xcb.h>
#include "render_readiness_gate.h"

#define TRY(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS && r_ != VK_SUBOPTIMAL_KHR) { \
  fprintf(stderr, "Vulkan failure %s => %d at %d\n", #x, r_, __LINE__); return 1; \
} } while (0)
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "Requirement failed: %s line %d\n", #x, __LINE__); return 1; } } while (0)

/* must match gpu_bridge.h gfg_gpu_stats (version 1) */
typedef struct {
    uint64_t version, bridges_created, sync_mode, real_presents, synthetic_presents,
        skipped_synthetic, te_busy, ring_busy, acquire_not_ready, holds, errors,
        present_path_host_pixel_reads, present_path_cpu_waits, teardown_cpu_waits,
        verify_checks, verify_distinct, verify_host_reads;
} gpu_stats;

static unsigned validation_errors, validation_warnings;
static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data,
        void *user) {
    (void)type; (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        fprintf(stderr, "VALIDATION ERROR: %s\n", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++validation_warnings;
        fprintf(stderr, "VALIDATION WARNING: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static unsigned env_uint(const char *name, unsigned def) {
    const char *v = getenv(name);
    return v && *v ? (unsigned)strtoul(v, NULL, 10) : def;
}
static int env_1(const char *name) {
    const char *v = getenv(name);
    return v && !strcmp(v, "1");
}
static uint64_t tick_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
static int compare_ns(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x<y?-1:x>y;
}
static int instance_ext(const char *layer, const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkExtensionProperties *p = calloc(n, sizeof(*p));
    int yes = 0;
    if (p && vkEnumerateInstanceExtensionProperties(layer, &n, p) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; ++i) if (!strcmp(p[i].extensionName, name)) yes = 1;
    free(p);
    return yes;
}
static int device_ext(VkPhysicalDevice physical, const char *ext) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *p = calloc(count, sizeof(*p));
    int yes = 0;
    if (p && vkEnumerateDeviceExtensionProperties(physical, NULL, &count, p) == VK_SUCCESS)
        for (uint32_t k = 0; k < count; k++) if (!strcmp(ext, p[k].extensionName)) { yes = 1; break; }
    free(p);
    return yes;
}
static int find_layer_cb(struct dl_phdr_info *info, size_t size, void *out) {
    (void)size;
    if (info->dlpi_name && strstr(info->dlpi_name, "libVkLayer_gfg_native.so")) {
        *(const char **)out = info->dlpi_name;
        return 1;
    }
    return 0;
}
static int read_gpu_stats(gpu_stats *st) {
    const char *path = NULL;
    dl_iterate_phdr(find_layer_cb, &path);
    void *layer = path ? dlopen(path, RTLD_NOW | RTLD_NOLOAD) : NULL;
    void (*get)(gpu_stats *) = layer ? (void (*)(gpu_stats *))dlsym(layer, "gfg_native_gpu_bridge_stats") : NULL;
    if (!get) { if (layer) dlclose(layer); return 0; }
    memset(st, 0, sizeof(*st)); get(st); dlclose(layer);
    return st->version == 1;
}
static int check_generation(const gpu_stats *before, const gpu_stats *after, uint32_t frames, unsigned generation) {
    const uint64_t real = after->real_presents - before->real_presents;
    const uint64_t synthetic = after->synthetic_presents - before->synthetic_presents;
    const uint64_t checks = after->verify_checks - before->verify_checks;
    const int valid = after->bridges_created - before->bridges_created == 1 &&
        real == frames && synthetic == (getenv("GFG_NATIVE_MODE") && !strcmp(getenv("GFG_NATIVE_MODE"), "2") ? 1ull : 2ull) * (frames - 1) &&
        after->skipped_synthetic == before->skipped_synthetic && after->errors == before->errors &&
        checks > 0 && after->verify_distinct - before->verify_distinct == checks;
    printf("GSF_LIFECYCLE_RETIRED generation=%u real=%llu synthetic=%llu pixel_checks=%llu status=%s\n",
        generation, (unsigned long long)real, (unsigned long long)synthetic,
        (unsigned long long)checks, valid ? "PASS" : "FAIL");
    return valid;
}
/* A managed X11 client sends ConfigureRequest via the WM, which may handle
 * it after our next geometry reply. A bare Xvfb server has no such round trip.
 * Wait for the actual client size, bounded independently of GPU work. */
static int wait_window_size(xcb_connection_t *xc, xcb_window_t window, uint32_t width, uint32_t height) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint32_t observed_width = 0, observed_height = 0;
    unsigned polls = 0;
    do {
        xcb_generic_error_t *error = NULL;
        xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, window), &error);
        ++polls;
        if (!geometry || error) {
            fprintf(stderr, "GSF_RESIZE geometry_error=%u\n", error ? error->error_code : 0);
            free(geometry); free(error); return 0;
        }
        observed_width = geometry->width; observed_height = geometry->height;
        free(geometry);
        if (observed_width == width && observed_height == height) {
            printf("GSF_RESIZE requested=%ux%u observed=%ux%u polls=%u status=PASS\n",
                width, height, observed_width, observed_height, polls);
            return 1;
        }
        const struct timespec delay = {.tv_nsec=10000000};
        nanosleep(&delay, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec-start.tv_sec)*1000000000ll + now.tv_nsec-start.tv_nsec < 3000000000ll);
    fprintf(stderr, "GSF_RESIZE requested=%ux%u observed=%ux%u polls=%u status=TIMEOUT\n",
        width, height, observed_width, observed_height, polls);
    return 0;
}
static uint32_t between(uint32_t x, uint32_t lo, uint32_t hi) { return x < lo ? lo : x > hi ? hi : x; }

/* textured blocks moving 4 px right and 1 px down per frame */
static void pattern(uint8_t *px, uint32_t w, uint32_t h, uint32_t frame) {
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t u = x + 4u * frame, v = y + frame;
            uint8_t *p = px + 4u * ((size_t)y * w + x);
            int check = ((u / 12u) ^ (v / 12u)) & 1u;
            p[0] = (uint8_t)(check ? 220 : 40) ^ (uint8_t)((u * 7u) & 31u);
            p[1] = (uint8_t)((u * 3u + v * 5u) & 255u);
            p[2] = (uint8_t)(check ? 60 : 200);
            p[3] = 255;
        }
}

/* App-rendered diagnostic label: static per case; not a generated-frame tag. */
static void badge(uint8_t *px, uint32_t w, uint32_t h, const char *text) {
    static const char keys[] = "ABFPS0123456789>";
    static const uint8_t rows[][7] = {
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
        {31,16,16,30,16,16,16},{30,17,17,30,16,16,16},{15,16,16,14,1,1,30},
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14},{16,8,4,2,4,8,16}};
    const uint32_t scale=w>=320 ? 3u : 1u;
    const uint32_t bar=9u*scale<h ? 9u*scale : h;
    for(uint32_t y=0;y<bar;y++)for(uint32_t x=0;x<w;x++) {
        uint8_t *p=px+4u*((size_t)y*w+x);p[0]=p[1]=p[2]=0;p[3]=255;
    }
    for(uint32_t n=0;text[n];n++) {
        const char *key=strchr(keys,text[n]);if(!key)continue;
        for(uint32_t y=0;y<7;y++)for(uint32_t x=0;x<5;x++)if(rows[key-keys][y]&(16u>>x))
            for(uint32_t sy=0;sy<scale;sy++)for(uint32_t sx=0;sx<scale;sx++) {
                uint32_t dx=scale+n*6u*scale+x*scale+sx,dy=scale+y*scale+sy;
                if(dx<w && dy<h){uint8_t *p=px+4u*((size_t)dy*w+dx);p[0]=p[1]=p[2]=255;}
            }
    }
}

int main(void) {
    const unsigned source_hz=env_uint("GFG_TEST_SOURCE_HZ",0);
    REQUIRE(source_hz<=240);
    uint32_t frames = env_uint("GFG_TEST_FRAMES", 24);
    const unsigned stress_seconds=env_uint("GFG_TEST_STRESS_SECONDS",0);
    const unsigned repeat_delay=env_uint("GFG_TEST_RENDER_READY_PERIOD_FRAMES",0);
    REQUIRE(!stress_seconds || (frames>=1000 && source_hz && stress_seconds<=1800));
    const unsigned render_delay_ms=env_uint("GFG_TEST_RENDER_READY_DELAY_MS",0);
    const unsigned render_delay_from=env_uint("GFG_TEST_RENDER_READY_FROM",0);
    const unsigned render_delay_until=env_uint("GFG_TEST_RENDER_READY_UNTIL",frames);
    REQUIRE(render_delay_ms<=1000 && render_delay_from<=render_delay_until && render_delay_until<=frames);
    const int lifecycle = env_1("GFG_TEST_LIFECYCLE");
    REQUIRE(!lifecycle || (frames >= 12 && frames % 3 == 0 &&
        env_1("GFG_TEST_EXPECT_GPU") && env_1("GFG_NATIVE_GPU_BRIDGE_VERIFY") &&
        !env_1("GFG_TEST_ALLOW_SKIPS")));
    gpu_stats generation_before = {0};
    unsigned generation = 0;
    const uint32_t want_w = env_uint("GFG_TEST_WIDTH", 96), want_h = env_uint("GFG_TEST_HEIGHT", 64);
    const int validation = env_1("GFG_TEST_VALIDATION");
    const int pace_idle = env_1("GFG_TEST_PACE_IDLE");
    const int proton_chain = env_1("GFG_TEST_PROTON_CHAIN");
    int screen_no = 0;
    xcb_connection_t *xc = xcb_connect(NULL, &screen_no);
    REQUIRE(xc && !xcb_connection_has_error(xc));
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(xc));
    for (int k = 0; k < screen_no; k++) xcb_screen_next(&it);
    REQUIRE(it.data);
    xcb_window_t window = xcb_generate_id(xc);
    const uint32_t opts[] = {it.data->black_pixel, XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(xc, XCB_COPY_FROM_PARENT, window, it.data->root, 0, 0, want_w, want_h, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, it.data->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, opts);
    const char *title = getenv("GFG_TEST_TITLE");
    if (!title) title = "GSF Vulkan diagnostic";
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
    xcb_map_window(xc, window);
    REQUIRE(xcb_flush(xc) > 0);

    const char *iext[6] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
    uint32_t niext = 2;
    const int surface_maintenance = proton_chain &&
        instance_ext(NULL, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
        instance_ext(NULL, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (surface_maintenance) {
        iext[niext++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        iext[niext++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    }
    int have_vf = 0;
    if (validation) {
        REQUIRE(instance_ext(NULL, "VK_EXT_debug_utils"));
        iext[niext++] = "VK_EXT_debug_utils";
        if (instance_ext("VK_LAYER_KHRONOS_validation", "VK_EXT_validation_features")) {
            iext[niext++] = "VK_EXT_validation_features";
            have_vf = 1;
        }
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "gfg-native-wsi-gpu-smoke", .pEngineName = "gfg-native-test",
        .apiVersion = VK_API_VERSION_1_2};
    VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT vf = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = enables};
    VkDebugUtilsMessengerCreateInfoEXT mci = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = have_vf ? &vf : NULL,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = on_message};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = validation ? &mci : NULL, .pApplicationInfo = &app,
        .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext};
    VkInstance instance = VK_NULL_HANDLE;
    TRY(vkCreateInstance(&ci, NULL, &instance));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        REQUIRE(create);
        mci.pNext = NULL;
        TRY(create(instance, &mci, NULL, &messenger));
        fprintf(stderr, "GFG native: validation messenger active sync_validation=%d\n", have_vf);
    }
    VkXcbSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection = xc, .window = window};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    TRY(vkCreateXcbSurfaceKHR(instance, &sci, NULL, &surface));

    uint32_t count = 0;
    TRY(vkEnumeratePhysicalDevices(instance, &count, NULL));
    REQUIRE(count > 0);
    VkPhysicalDevice *phys = calloc(count, sizeof(*phys));
    REQUIRE(phys);
    TRY(vkEnumeratePhysicalDevices(instance, &count, phys));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count && !physical; i++) {
        if (!device_ext(phys[i], "VK_KHR_swapchain")) continue;
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qcount, NULL);
        VkQueueFamilyProperties *fam = calloc(qcount, sizeof(*fam));
        REQUIRE(fam);
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qcount, fam);
        for (uint32_t q = 0; q < qcount; q++) {
            VkBool32 ok = VK_FALSE;
            if (fam[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                TRY(vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], q, surface, &ok));
            if (ok) { physical = phys[i]; family = q; break; }
        }
        free(fam);
    }
    free(phys);
    REQUIRE(physical && family != UINT32_MAX);
    VkPhysicalDeviceProperties device_info;
    vkGetPhysicalDeviceProperties(physical, &device_info);
    printf("GSF_DEVICE name=%s vendor=0x%x device=0x%x api=%u driver=%u\n",
        device_info.deviceName, device_info.vendorID, device_info.deviceID,
        device_info.apiVersion, device_info.driverVersion);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *dext[4] = {"VK_KHR_swapchain"};
    uint32_t dext_count = 1;
    const int mutable_format=env_1("GFG_TEST_MUTABLE_SWAPCHAIN");
    if(mutable_format) {
        REQUIRE(device_ext(physical,VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME));
        dext[dext_count++]=VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME;
    }
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    int maintenance_chain = 0;
    if (proton_chain) {
        REQUIRE(device_ext(physical, VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME));
        if (surface_maintenance && device_ext(physical, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
            VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &maintenance};
            vkGetPhysicalDeviceFeatures2(physical, &features);
            maintenance_chain = maintenance.swapchainMaintenance1 != 0;
        }
        if (env_1("GFG_TEST_REQUIRE_MAINTENANCE1")) REQUIRE(maintenance_chain);
        if (maintenance_chain) dext[dext_count++] = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        else puts("GFG native: SKIP maintenance1-fence WSI (ICD unsupported); exercising incremental-present chain");
        dext[dext_count++] = VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME;
    }
    /* An app-owned feature chain the layer must preserve exactly. */
    const int chain12 = env_1("GFG_TEST_CHAIN_VK12");
    const int chain12_disabled = env_1("GFG_TEST_CHAIN_VK12_DISABLED");
    VkPhysicalDeviceVulkan12Features f12_local = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = chain12 && !chain12_disabled ? VK_TRUE : VK_FALSE};
    void *feature_page = NULL;
    VkPhysicalDeviceVulkan12Features *f12 = &f12_local;
    if (chain12 && env_1("GFG_TEST_READONLY_FEATURES")) {
        feature_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(feature_page != MAP_FAILED);
        memcpy(feature_page, &f12_local, sizeof(f12_local));
        REQUIRE(mprotect(feature_page, 4096, PROT_READ) == 0);
        f12 = feature_page;
    }
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = chain12 ? f12 : NULL,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
        .enabledExtensionCount = dext_count, .ppEnabledExtensionNames = dext};
    if (maintenance_chain) {
        maintenance.pNext = (void *)di.pNext;
        di.pNext = &maintenance;
    }
    VkDevice device = VK_NULL_HANDLE;
    TRY(vkCreateDevice(physical, &di, NULL, &device));
    REQUIRE(f12->timelineSemaphore == (chain12 && !chain12_disabled ? VK_TRUE : VK_FALSE));
    if (feature_page) REQUIRE(munmap(feature_page, 4096) == 0);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);
    REQUIRE(queue);

    VkSurfaceCapabilitiesKHR caps;
    TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    REQUIRE(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        ext.width = between(want_w, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = between(want_h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    uint32_t nformats = 0;
    TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nformats, NULL));
    VkSurfaceFormatKHR *formats = calloc(nformats, sizeof(*formats));
    REQUIRE(formats && nformats);
    TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nformats, formats));
    VkSurfaceFormatKHR color = formats[0];
    const int want_srgb=env_1("GFG_TEST_SRGB") || env_1("GFG_NATIVE_TEST_SRGB");
    const VkFormat wanted = env_1("GFG_TEST_RGBA") ?
        (want_srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM):
        (want_srgb?VK_FORMAT_B8G8R8A8_SRGB:VK_FORMAT_B8G8R8A8_UNORM);
    for (uint32_t i = 0; i < nformats; i++)
        if (formats[i].format == wanted) { color = formats[i]; break; }
    free(formats);
    if(env_1("GFG_TEST_RGBA"))REQUIRE(color.format==wanted);
    uint32_t minimum = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    if (caps.maxImageCount && minimum > caps.maxImageCount) minimum = caps.maxImageCount;
    VkSwapchainCreateInfoKHR swci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface, .minImageCount = minimum, .imageFormat = color.format,
        .imageColorSpace = color.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = env_1("GFG_TEST_MAILBOX_SWAPCHAIN")
            ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE};
    const VkPresentModeKHR compatible_mode = swci.presentMode;
    const VkSwapchainPresentModesCreateInfoEXT compatible = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
        .presentModeCount = 1, .pPresentModes = &compatible_mode};
    if (maintenance_chain) swci.pNext = &compatible;
    const VkFormat alias=color.format==VK_FORMAT_B8G8R8A8_UNORM?VK_FORMAT_B8G8R8A8_SRGB:
        color.format==VK_FORMAT_B8G8R8A8_SRGB?VK_FORMAT_B8G8R8A8_UNORM:
        color.format==VK_FORMAT_R8G8B8A8_UNORM?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat view_formats[2]={color.format,alias};
    const VkImageFormatListCreateInfo format_list={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext=swci.pNext,.viewFormatCount=2,.pViewFormats=view_formats};
    if(mutable_format) {
        REQUIRE(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        swci.imageUsage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swci.flags|=VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        swci.pNext=&format_list;
    }
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        for (uint32_t bit = 1; bit <= 8; bit *= 2)
            if (caps.supportedCompositeAlpha & bit) { swci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)bit; break; }
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    TRY(vkCreateSwapchainKHR(device, &swci, NULL, &swapchain));
    uint32_t image_count = 0;
    TRY(vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL));
    VkImage *images = calloc(image_count, sizeof(*images));
    REQUIRE(images);
    TRY(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images));
    if(mutable_format) {
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=images[0],
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=alias,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        VkImageView view=VK_NULL_HANDLE;
        TRY(vkCreateImageView(device,&vi,NULL,&view));
        vkDestroyImageView(device,view,NULL);
        REQUIRE(swci.flags==VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR && swci.pNext==&format_list);
        REQUIRE(format_list.viewFormatCount==2 && view_formats[0]==color.format && view_formats[1]==alias);
        puts("GSF_MUTABLE_SWAPCHAIN unorm_srgb_alias_view=PASS original_metadata_unchanged=PASS");
    }
    fprintf(stderr, "GFG native: gpu smoke %ux%u format=%d min_images=%u images=%u frames=%u\n",
            ext.width, ext.height, (int)color.format, minimum, image_count, frames);

    /* app resources: 2 frames in flight */
    enum { INFLIGHT = 2 };
    const VkDeviceSize bytes = (VkDeviceSize)(ext.width + (lifecycle ? 32u : 0u)) *
        (ext.height + (lifecycle ? 16u : 0u)) * 4u;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    VkBuffer staging[INFLIGHT];
    VkDeviceMemory staging_mem[INFLIGHT];
    void *staging_map[INFLIGHT];
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    TRY(vkCreateCommandPool(device, &pci, NULL, &pool));
    VkCommandBuffer cmd[INFLIGHT];
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = INFLIGHT};
    TRY(vkAllocateCommandBuffers(device, &cai, cmd));
    if(env_1("GFG_TEST_CB_HANDLES"))
        fprintf(stderr,"GFG test app command buffers: %p %p\n",(void *)cmd[0],(void *)cmd[1]);
    VkFence fences[INFLIGHT];
    VkFence *present_fences = maintenance_chain ? calloc(frames, sizeof(*present_fences)) : NULL;
    if (maintenance_chain) {
        REQUIRE(present_fences);
        const VkFenceCreateInfo unsignaled = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (uint32_t i = 0; i < frames; ++i)
            TRY(vkCreateFence(device, &unsignaled, NULL, &present_fences[i]));
    }
    VkSemaphore acquired[INFLIGHT];
    VkSemaphoreCreateInfo semci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < INFLIGHT; ++i) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        TRY(vkCreateBuffer(device, &bi, NULL, &staging[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, staging[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount; ++t)
            if ((req.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = t; break; }
        REQUIRE(type != UINT32_MAX);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size, .memoryTypeIndex = type};
        TRY(vkAllocateMemory(device, &mai, NULL, &staging_mem[i]));
        TRY(vkBindBufferMemory(device, staging[i], staging_mem[i], 0));
        TRY(vkMapMemory(device, staging_mem[i], 0, VK_WHOLE_SIZE, 0, &staging_map[i]));
        TRY(vkCreateFence(device, &fci, NULL, &fences[i]));
        TRY(vkCreateSemaphore(device, &semci, NULL, &acquired[i]));
    }
    VkSemaphore *rendered = calloc(image_count, sizeof(*rendered)); /* per image */
    REQUIRE(rendered);
    for (uint32_t i = 0; i < image_count; ++i) TRY(vkCreateSemaphore(device, &semci, NULL, &rendered[i]));
    render_readiness_gate *render_gate=render_delay_ms?rrg_start(device):NULL;
    REQUIRE(!render_delay_ms || (render_gate && !lifecycle));
    const char *render_trace_path=getenv("GFG_TEST_RENDER_READY_TRACE");
    FILE *render_trace=render_trace_path?fopen(render_trace_path,"w"):NULL;
    REQUIRE(!render_trace_path || render_trace);

    struct timespec source_origin;
    clock_gettime(CLOCK_MONOTONIC, &source_origin);
    uint64_t source_next = (uint64_t)source_origin.tv_sec*1000000000ull + source_origin.tv_nsec;
    uint64_t source_first = 0, source_last = 0, steady_first = 0;
    const int absolute_pacing = env_1("GFG_TEST_ABSOLUTE_PACING");
    const int variable_source=env_1("GFG_TEST_VARIABLE_SOURCE");
    uint64_t *acquire_ns=calloc(frames,sizeof(*acquire_ns)),*present_ns=calloc(frames,sizeof(*present_ns));
    REQUIRE(acquire_ns && present_ns);
    for (uint32_t frame = 0; frame < frames; frame++) {
        const uint32_t f = frame % INFLIGHT;
        TRY(vkWaitForFences(device, 1, &fences[f], VK_TRUE, UINT64_MAX)); /* app-side wait */
        TRY(vkResetFences(device, 1, &fences[f]));
        pattern(staging_map[f], ext.width, ext.height, frame);
        const char *label = getenv("GFG_TEST_BADGE");
        if (label) badge(staging_map[f], ext.width, ext.height, label);
        uint32_t index = UINT32_MAX;
        uint64_t api_start=tick_ns();
        TRY(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[f], VK_NULL_HANDLE, &index));
        acquire_ns[frame]=tick_ns()-api_start;
        REQUIRE(index < image_count);
        TRY(vkResetCommandBuffer(cmd[f], 0));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(cmd[f], &begin));
        const int delayed=render_gate && frame>=render_delay_from && frame<render_delay_until &&
            (!repeat_delay || frame%repeat_delay>=repeat_delay/2);
        if(delayed) {
            vkCmdWaitEvents(cmd[f],1,&render_gate->events[f],VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,NULL,0,NULL,0,NULL);
        }
        VkImageMemoryBarrier pre = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[index], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd[f], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &pre);
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {ext.width, ext.height, 1}};
        vkCmdCopyBufferToImage(cmd[f], staging[f], images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier post = pre;
        post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        post.dstAccessMask = 0;
        post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd[f], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 0, NULL, 1, &post);
        TRY(vkEndCommandBuffer(cmd[f]));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired[f], .pWaitDstStageMask = &stage,
            .commandBufferCount = 1, .pCommandBuffers = &cmd[f],
            .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered[index]};
        uint64_t render_submit_ns=tick_ns();
        if(delayed)REQUIRE(rrg_arm(render_gate,f,(uint64_t)render_delay_ms*1000000ull));
        TRY(vkQueueSubmit(queue, 1, &si, fences[f]));
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &rendered[index],
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index};
        const VkPresentModeKHR app_mode = swci.presentMode;
        const VkRectLayerKHR rect = {.extent = ext};
        const VkPresentRegionKHR present_region = {.rectangleCount = 1, .pRectangles = &rect};
        const VkPresentRegionsKHR regions = {.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,
            .swapchainCount = 1, .pRegions = &present_region};
        const VkSwapchainPresentModeInfoEXT dynamic = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
            .pNext = &regions, .swapchainCount = 1, .pPresentModes = &app_mode};
        const VkSwapchainPresentFenceInfoEXT present_fence = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
            .pNext = &dynamic, .swapchainCount = 1,
            .pFences = maintenance_chain ? &present_fences[frame] : NULL};
        if (proton_chain) pi.pNext = maintenance_chain ? (const void *)&present_fence : &regions;
        const uint32_t device_mask=1;
        const VkDeviceGroupPresentInfoKHR group={.sType=VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,
            .pNext=pi.pNext,.swapchainCount=1,.pDeviceMasks=&device_mask,.mode=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR};
        if(env_1("GFG_TEST_UNKNOWN_PRESENT") && frame==frames/2) pi.pNext=&group;
        struct timespec source_now;
        clock_gettime(CLOCK_MONOTONIC, &source_now);
        source_last = (uint64_t)source_now.tv_sec*1000000000ull + source_now.tv_nsec;
        if(render_trace)fprintf(render_trace,"{\"event\":\"source\",\"frame\":%u,\"submit_ns\":%llu,\"present_ns\":%llu,\"injected_delay_ms\":%u}\n",
            frame,(unsigned long long)render_submit_ns,(unsigned long long)source_last,delayed?render_delay_ms:0);
        else if(render_gate)printf("GSF_RENDER_READY_SOURCE frame=%u submit_ns=%llu present_ns=%llu injected_delay_ms=%u\n",
            frame,(unsigned long long)render_submit_ns,(unsigned long long)source_last,delayed?render_delay_ms:0);
        if (!frame) source_first = source_last;
        if (frame == 2) steady_first = source_last;
        VkResult presented=vkQueuePresentKHR(queue,&pi);
        present_ns[frame]=tick_ns()-source_last;
        REQUIRE(present_fence.pNext == &dynamic && dynamic.pNext == &regions);
        REQUIRE(app_mode == swci.presentMode && present_region.pRectangles == &rect);
        if(env_1("GFG_TEST_EXPECT_RECREATE") && frame+1==frames)
            REQUIRE(presented==VK_ERROR_OUT_OF_DATE_KHR);
        else TRY(presented);
        if (lifecycle && (frame + 1) % (frames / 3) == 0 && frame + 1 < frames) {
            /* First replacement: no app queue drain before create, exercising
             * the layer's oldSwapchain output retirement. Second: resize only
             * after draining, so the X server cannot invalidate queued output. */
            if (generation == 1) {
                TRY(vkQueueWaitIdle(queue));
                const uint32_t size[2] = {want_w + 32u, want_h + 16u};
                xcb_configure_window(xc, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
                xcb_flush(xc);
                REQUIRE(wait_window_size(xc, window, size[0], size[1]));
                TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
                ext = caps.currentExtent.width == UINT32_MAX ? (VkExtent2D){size[0], size[1]} : caps.currentExtent;
                REQUIRE(ext.width == size[0] && ext.height == size[1]);
                swci.imageExtent = ext;
                swci.preTransform = caps.currentTransform;
            }
            VkSwapchainKHR replacement = VK_NULL_HANDLE;
            swci.oldSwapchain = swapchain;
            TRY(vkCreateSwapchainKHR(device, &swci, NULL, &replacement));
            TRY(vkQueueWaitIdle(queue));
            vkDestroySwapchainKHR(device, swapchain, NULL);
            gpu_stats completed;
            REQUIRE(read_gpu_stats(&completed));
            REQUIRE(check_generation(&generation_before, &completed, frames / 3, generation));
            generation_before = completed;
            ++generation;
            for (uint32_t i = 0; i < image_count; ++i) vkDestroySemaphore(device, rendered[i], NULL);
            free(rendered); free(images);
            swapchain = replacement;
            swci.oldSwapchain = VK_NULL_HANDLE;
            TRY(vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL));
            images = calloc(image_count, sizeof(*images));
            rendered = calloc(image_count, sizeof(*rendered));
            REQUIRE(images && rendered);
            TRY(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images));
            for (uint32_t i = 0; i < image_count; ++i)
                TRY(vkCreateSemaphore(device, &semci, NULL, &rendered[i]));
            printf("GSF_LIFECYCLE_READY generation=%u extent=%ux%u old_retired=1\n", generation, ext.width, ext.height);
        }
        if (pace_idle) TRY(vkQueueWaitIdle(queue)); /* app-side pacing, outside the layer */
        if(source_hz) {
            struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);
            if (absolute_pacing) {
                /* A variable source, not a fixed 30->60 assumption: ordinary
                 * 30..45 FPS, short delivery bursts, and a scene-change gap. */
                uint64_t period = 1000000000ull/source_hz;
                if(variable_source) {
                    if(stress_seconds) {
                        const unsigned rates[]={5,30,45,60,90,45,5};
                        period=1000000000ull/rates[(frame/60)%7]+(frame%2?200000:0);
                        if(frame%600==599)period+=300000000ull;
                    } else period=frame%40<4?1000000ull:
                        frame%40==20?120000000ull:1000000000ull/(30+(frame/5)%16);
                }
                const uint64_t now = (uint64_t)deadline.tv_sec*1000000000ull + deadline.tv_nsec;
                source_next += period;
                /* Do not burst through missed source slots after cold startup. */
                if (source_next < now) source_next = now + period;
                deadline.tv_sec = (time_t)(source_next/1000000000ull);
                deadline.tv_nsec = (long)(source_next%1000000000ull);
            } else {
                deadline.tv_nsec+=1000000000l/source_hz;
                if(deadline.tv_nsec>=1000000000l){++deadline.tv_sec;deadline.tv_nsec-=1000000000l;}
            }
            clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&deadline,NULL);
        }
        if(stress_seconds && tick_ns()-source_first>=(uint64_t)stress_seconds*1000000000ull) {
            frames=frame+1;break;
        }
        if(stress_seconds && frame%60==0) {
            struct rusage usage;
            if(!getrusage(RUSAGE_SELF,&usage))printf("GSF_STRESS_RESOURCE frame=%u elapsed_ms=%llu peak_rss_kib=%ld\n",
                frame,(unsigned long long)((tick_ns()-source_first)/1000000ull),usage.ru_maxrss);
        }
    }
    printf("GSF_SOURCE_RATE frames=%u requested_hz=%u measured_hz=%.6f steady_hz=%.6f absolute=%d\n",
        frames, source_hz, source_last>source_first ?
        (double)(frames-1)*1e9/(double)(source_last-source_first) : 0.0,
        frames>3 && source_last>steady_first ? (double)(frames-3)*1e9/(double)(source_last-steady_first) : 0.0, absolute_pacing);
    if(frames>3) {
        unsigned n=frames-3;
        qsort(acquire_ns+3,n,sizeof(*acquire_ns),compare_ns);
        qsort(present_ns+3,n,sizeof(*present_ns),compare_ns);
        printf("GSF_API_LATENCY variable=%d samples=%u acquire_p50_ms=%.6f acquire_p95_ms=%.6f present_p50_ms=%.6f present_p95_ms=%.6f\n",
            variable_source,n,acquire_ns[3+n/2]/1e6,acquire_ns[3+(n-1)*95/100]/1e6,
            present_ns[3+n/2]/1e6,present_ns[3+(n-1)*95/100]/1e6);
    }
    free(acquire_ns);free(present_ns);
    if(env_1("GFG_TEST_RECREATE")) {
        VkSwapchainKHR replacement=VK_NULL_HANDLE;
        swci.oldSwapchain=swapchain;
        TRY(vkCreateSwapchainKHR(device,&swci,NULL,&replacement));
        TRY(vkQueueWaitIdle(queue));
        vkDestroySwapchainKHR(device,swapchain,NULL);
        swapchain=replacement;
        puts("GFG native: recreate with pending output + queue idle drain PASS");
    } else if(env_1("GFG_TEST_QUEUE_IDLE_ONLY")) TRY(vkQueueWaitIdle(queue));
    else TRY(vkDeviceWaitIdle(device));
    REQUIRE(rrg_stop(render_gate,render_trace));
    if(render_trace)REQUIRE(fclose(render_trace)==0);
    if (maintenance_chain) {
        TRY(vkWaitForFences(device, frames, present_fences, VK_TRUE, 5000000000ull));
        for (uint32_t i = 0; i < frames; ++i) {
            TRY(vkGetFenceStatus(device, present_fences[i]));
            vkDestroyFence(device, present_fences[i], NULL);
        }
        free(present_fences);
        printf("GFG native: Proton present chain fences=%u original_metadata_unchanged=PASS\n", frames);
    }
    if (proton_chain && !maintenance_chain)
        puts("GFG native: Proton present chain regions=PASS fences=SKIPPED original_metadata_unchanged=PASS");
    vkDestroySwapchainKHR(device, swapchain, NULL); /* retires the bridge (final verify) */

    int failed = 0;
    if (env_1("GFG_TEST_EXPECT_GPU")) {
        const char *path = NULL;
        dl_iterate_phdr(find_layer_cb, &path);
        void *layer = path ? dlopen(path, RTLD_NOW | RTLD_NOLOAD) : NULL;
        void (*get)(gpu_stats *) = layer ? (void (*)(gpu_stats *))dlsym(layer, "gfg_native_gpu_bridge_stats") : NULL;
        REQUIRE(get);
        const char *expected_layer = getenv("GFG_TEST_EXPECT_LAYER_PATH");
        if (expected_layer) {
            char *actual = realpath(path, NULL), *expected = realpath(expected_layer, NULL);
            REQUIRE(actual && expected && !strcmp(actual, expected));
            printf("GSF_LOADED_LAYER path=%s verified=1\n", actual);
            free(actual); free(expected);
        }
        gpu_stats st;
        memset(&st, 0, sizeof st);
        get(&st);
        printf("GFG native: gpu stats bridges=%llu sync_mode=%llu real=%llu synthetic=%llu skipped=%llu"
               " te_busy=%llu ring_busy=%llu acquire_not_ready=%llu holds=%llu errors=%llu"
               " present_path_host_pixel_reads=%llu present_path_cpu_waits=%llu teardown_cpu_waits=%llu"
               " verify_checks=%llu verify_distinct=%llu verify_host_reads=%llu\n",
               (unsigned long long)st.bridges_created, (unsigned long long)st.sync_mode,
               (unsigned long long)st.real_presents, (unsigned long long)st.synthetic_presents,
               (unsigned long long)st.skipped_synthetic, (unsigned long long)st.te_busy,
               (unsigned long long)st.ring_busy, (unsigned long long)st.acquire_not_ready,
               (unsigned long long)st.holds, (unsigned long long)st.errors,
               (unsigned long long)st.present_path_host_pixel_reads,
               (unsigned long long)st.present_path_cpu_waits, (unsigned long long)st.teardown_cpu_waits,
               (unsigned long long)st.verify_checks, (unsigned long long)st.verify_distinct,
               (unsigned long long)st.verify_host_reads);
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "GPU smoke check failed: %s\n", #c); failed = 1; } } while (0)
        CHECK(st.version == 1);
        CHECK(st.bridges_created == (lifecycle ? 3u : 1u));
        if (lifecycle) CHECK(check_generation(&generation_before, &st, frames / 3, generation));
        CHECK(st.real_presents == frames);
        CHECK(st.synthetic_presents + st.skipped_synthetic == (getenv("GFG_NATIVE_MODE") && !strcmp(getenv("GFG_NATIVE_MODE"),"2")?1ull:2ull) * (frames - (lifecycle ? 3u : 1u)));
        if (!env_1("GFG_TEST_ALLOW_SKIPS")) CHECK(st.skipped_synthetic == 0);
        CHECK(st.errors == (env_1("GFG_TEST_EXPECT_FAULT")?1u:0u));
        CHECK(st.present_path_host_pixel_reads == 0);
        CHECK(st.present_path_cpu_waits == 0);
        if (env_1("GFG_NATIVE_GPU_BRIDGE_VERIFY")) {
            CHECK(st.verify_checks > 0);
            CHECK(st.verify_distinct == st.verify_checks);
        }
    }
    if (validation) {
        printf("GFG native: validation errors=%u warnings=%u\n", validation_errors, validation_warnings);
        if (validation_errors) failed = 1;
    }
    for (uint32_t i = 0; i < image_count; ++i) vkDestroySemaphore(device, rendered[i], NULL);
    for (int i = 0; i < INFLIGHT; ++i) {
        vkDestroySemaphore(device, acquired[i], NULL);
        vkDestroyFence(device, fences[i], NULL);
        vkDestroyBuffer(device, staging[i], NULL);
        vkFreeMemory(device, staging_mem[i], NULL);
    }
    vkDestroyCommandPool(device, pool, NULL);
    free(rendered);
    free(images);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    if (messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(instance, messenger, NULL);
    }
    vkDestroyInstance(instance, NULL);
    xcb_destroy_window(xc, window);
    xcb_disconnect(xc);
    if (failed) { puts("GFG native: GPU bridge WSI smoke FAIL"); return 1; }
    printf("GFG native: %u real frames GPU bridge WSI smoke PASS\n", frames);
    return 0;
}

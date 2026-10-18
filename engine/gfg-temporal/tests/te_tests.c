/* Native tests of the Temporal Engine.  They run real compute work on the
 * selected Vulkan device (Mesa lavapipe in CI) and check results, not mocks. */
#define _GNU_SOURCE
#include "te_engine.h"
#include "te_scheduler.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures, g_checks;

#define EXPECT(cond, ...)                                                   \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, " [%s]\n", te_last_error());                    \
        }                                                                   \
    } while (0)

enum { W = 256, H = 160 };

static uint8_t clamp8(double v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

/* Smooth textured pattern sampled at (x - ox, y - oy): enough detail for ME. */
static void pattern(uint8_t* rgb, double ox, double oy, unsigned seed) {
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double u = x - ox, v = y - oy;
            double s = seed * 0.37;
            double r = 128 + 60 * sin(u * 0.21 + s) * cos(v * 0.17) + 40 * sin((u + v) * 0.09 + 2 * s);
            double g = 128 + 55 * sin(v * 0.23 - s) + 45 * cos(u * 0.13 + v * 0.05);
            double b = 128 + 70 * cos(u * 0.07 - v * 0.19 + s);
            uint8_t* p = rgb + 3 * (y * W + x);
            p[0] = clamp8(r);
            p[1] = clamp8(g);
            p[2] = clamp8(b);
        }
    }
}

static void noise(uint8_t* rgb, unsigned seed) {
    for (int i = 0; i < W * H * 3; ++i) {
        seed = seed * 1103515245u + 12345u;
        rgb[i] = (uint8_t)(seed >> 16);
    }
}

static double psnr_rect(const uint8_t* a, const uint8_t* b, int x0, int y0, int x1, int y1) {
    double se = 0;
    long n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            for (int c = 0; c < 3; ++c) {
                double d = (double)a[3 * (y * W + x) + c] - b[3 * (y * W + x) + c];
                se += d * d;
                n++;
            }
    double mse = se / n;
    return mse <= 1e-9 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

static te_context* make_ctx(te_device* dev, uint32_t outputs, int adaptive, int force) {
    te_context_desc d;
    te_context_desc_default(&d, W, H, outputs);
    d.host_io = 1;
    d.profile = 1;
    d.params.adaptive = adaptive;
    d.params.force_class = force;
    te_context* ctx = NULL;
    if (te_context_create(dev, &d, &ctx) != VK_SUCCESS) return NULL;
    return ctx;
}

static int g_lmc_tier;   /* LMC cost tier of every submit (te_submit.lmc_tier) */

static int run_pair(te_context* ctx, uint32_t slot, const uint8_t* frame, uint32_t count, const float* ts) {
    if (te_upload_rgb(ctx, slot, frame) != VK_SUCCESS) return 0;
    te_submit s = {.slot = slot, .count = count, .download = 1, .lmc_tier = g_lmc_tier};
    for (uint32_t i = 0; i < count; ++i) s.timestamps[i] = ts[i];
    return te_submit_frame(ctx, &s) == VK_SUCCESS && te_wait_idle(ctx) == VK_SUCCESS;
}

static void test_static_scene(te_device* dev) {
    te_context* ctx = make_ctx(dev, 2, 1, 2);
    EXPECT(ctx, "context");
    if (!ctx) return;
    uint8_t* a = malloc(W * H * 3);
    uint8_t* o = malloc(W * H * 3);
    pattern(a, 0, 0, 1);
    const float ts[2] = {1.0f / 3, 2.0f / 3};
    EXPECT(run_pair(ctx, 0, a, 0, ts), "first frame");
    EXPECT(run_pair(ctx, 1, a, 2, ts), "static pair");
    te_pair_stats st;
    EXPECT(te_last_stats(ctx, &st) == VK_SUCCESS, "stats");
    EXPECT(st.tiles[0][0] == st.tile_total, "static scene: all tiles REUSE (%u of %u)", st.tiles[0][0], st.tile_total);
    EXPECT(st.timed, "timestamps available");
    te_download_rgb(ctx, 1, o);
    EXPECT(memcmp(a, o, W * H * 3) == 0, "REUSE output is the real frame bit for bit");
    free(a);
    free(o);
    te_context_destroy(ctx);
}

static void test_translation(te_device* dev) {
    te_context* ctx = make_ctx(dev, 1, 1, 2);
    EXPECT(ctx, "context");
    if (!ctx) return;
    uint8_t *a = malloc(W * H * 3), *b = malloc(W * H * 3), *mid = malloc(W * H * 3), *o = malloc(W * H * 3);
    pattern(a, 0, 0, 2);
    pattern(b, 6, 2, 2);
    pattern(mid, 3, 1, 2);
    const float ts[1] = {0.5f};
    EXPECT(run_pair(ctx, 0, a, 0, ts), "first frame");
    EXPECT(run_pair(ctx, 1, b, 1, ts), "pair");
    uint32_t tx, ty;
    uint32_t n = te_tile_count(ctx, &tx, &ty);
    float* v = malloc(n * 8);
    EXPECT(te_read_vectors(ctx, v) == VK_SUCCESS, "vectors");
    int good = 0, interior = 0;
    for (uint32_t y = 1; y + 1 < ty; ++y)
        for (uint32_t x = 1; x + 1 < tx; ++x) {
            float vx = v[2 * (y * tx + x)], vy = v[2 * (y * tx + x) + 1];
            interior++;
            good += fabsf(vx - 6.0f) <= 0.5f && fabsf(vy - 2.0f) <= 0.5f;
        }
    EXPECT(good * 10 >= interior * 9, "translation (6,2) found on %d of %d interior tiles", good, interior);
    te_download_rgb(ctx, 0, o);
    double q = psnr_rect(o, mid, 16, 16, W - 16, H - 16);
    double rep = psnr_rect(a, mid, 16, 16, W - 16, H - 16);
    EXPECT(q > 34.0 && q > rep + 8.0, "t=0.5 reconstruction PSNR %.2f dB (repeat %.2f dB)", q, rep);
    te_pair_stats st;
    te_last_stats(ctx, &st);
    EXPECT(st.tiles[0][0] + st.tiles[0][1] + st.tiles[0][2] == st.tile_total, "every tile has one class");
    EXPECT(st.tiles[0][1] > 0, "pure translation uses LIGHT tiles (%u)", st.tiles[0][1]);
    free(v);
    free(a);
    free(b);
    free(mid);
    free(o);
    te_context_destroy(ctx);
}

static void test_scene_cut(te_device* dev) {
    te_context* ctx = make_ctx(dev, 2, 1, 2);
    EXPECT(ctx, "context");
    if (!ctx) return;
    uint8_t *a = malloc(W * H * 3), *b = malloc(W * H * 3), *o = malloc(W * H * 3);
    noise(a, 7);
    noise(b, 99);
    const float ts[2] = {0.25f, 0.75f};
    run_pair(ctx, 0, a, 0, ts);
    run_pair(ctx, 1, b, 2, ts);
    te_pair_stats st;
    te_last_stats(ctx, &st);
    EXPECT(st.scene_cut == 1, "unrelated frames detected as a scene cut");
    te_download_rgb(ctx, 0, o);
    EXPECT(memcmp(o, a, W * H * 3) == 0, "hold before the midpoint shows the older real frame");
    te_download_rgb(ctx, 1, o);
    EXPECT(memcmp(o, b, W * H * 3) == 0, "hold after the midpoint shows the newer real frame");
    free(a);
    free(b);
    free(o);
    te_context_destroy(ctx);
}

static void test_forced_modes(te_device* dev) {
    uint8_t *a = malloc(W * H * 3), *b = malloc(W * H * 3);
    pattern(a, 0, 0, 3);
    pattern(b, 4, -3, 3);
    const float ts[1] = {0.5f};
    for (int c = 0; c < 3; ++c) {
        te_context* ctx = make_ctx(dev, 1, 0, c);
        EXPECT(ctx, "context");
        if (!ctx) continue;
        run_pair(ctx, 0, a, 0, ts);
        run_pair(ctx, 1, b, 1, ts);
        te_pair_stats st;
        te_last_stats(ctx, &st);
        EXPECT(st.tiles[0][c] == st.tile_total, "forced class %d covers all tiles (%u/%u/%u)", c, st.tiles[0][0],
               st.tiles[0][1], st.tiles[0][2]);
        EXPECT(st.profiled && st.frame_ms[0][TE_FRAME_TOTAL] > 0.0, "profiled frame time measured");
        te_context_destroy(ctx);
    }
    free(a);
    free(b);
}

static void test_hysteresis(te_device* dev) {
    /* moving pattern for 3 pairs, then static: REUSE may only appear after
     * two consecutive eligible pairs */
    te_context* ctx = make_ctx(dev, 1, 1, 2);
    EXPECT(ctx, "context");
    if (!ctx) return;
    uint8_t* f = malloc(W * H * 3);
    const float ts[1] = {0.5f};
    uint32_t reuse[8];
    for (int k = 0; k < 8; ++k) {
        double shift = k < 4 ? 5.0 * k : 15.0;
        pattern(f, shift, 0, 4);
        run_pair(ctx, (uint32_t)(k % 2), f, k ? 1 : 0, ts);
        te_pair_stats st;
        if (k) {
            te_last_stats(ctx, &st);
            reuse[k] = st.tiles[0][0];
        }
    }
    /* pairs 1..3 move (frames shift by 5 px), pairs 4.. are static */
    EXPECT(reuse[3] == 0, "moving pair has no REUSE tiles (%u)", reuse[3]);
    EXPECT(reuse[4] == 0, "first static pair keeps the previous class (%u REUSE)", reuse[4]);
    EXPECT(reuse[5] > 0, "second consecutive static pair downgrades to REUSE (%u)", reuse[5]);
    free(f);
    te_context_destroy(ctx);
}

static void test_invalid_inputs(te_device* dev) {
    te_context_desc d;
    te_context_desc_default(&d, W, H, 1);
    te_context* ctx = NULL;
    d.params.tile = 12;
    EXPECT(te_context_create(dev, &d, &ctx) != VK_SUCCESS && !ctx, "tile 12 is refused");
    te_context_desc_default(&d, W, H, 5);
    EXPECT(te_context_create(dev, &d, &ctx) != VK_SUCCESS && !ctx, "5 outputs are refused");
    ctx = make_ctx(dev, 2, 1, 2);
    if (!ctx) return;
    uint8_t* f = malloc(W * H * 3);
    pattern(f, 0, 0, 5);
    te_upload_rgb(ctx, 0, f);
    te_submit s = {.slot = 0, .count = 0};
    EXPECT(te_submit_frame(ctx, &s) == VK_SUCCESS, "first frame");
    te_submit bad = {.slot = 0, .count = 1, .timestamps = {0.5f}};
    EXPECT(te_submit_frame(ctx, &bad) != VK_SUCCESS, "same slot twice is refused");
    te_submit bad2 = {.slot = 1, .count = 2, .timestamps = {0.6f, 0.4f}};
    EXPECT(te_submit_frame(ctx, &bad2) != VK_SUCCESS, "decreasing timestamps are refused");
    te_submit bad3 = {.slot = 1, .count = 1, .timestamps = {1.0f}};
    EXPECT(te_submit_frame(ctx, &bad3) != VK_SUCCESS, "t = 1 is refused");
    te_wait_idle(ctx);
    free(f);
    te_context_destroy(ctx);
}

static void test_lifetime(te_device* dev) {
    /* repeated create/run/destroy must not leak: the validation layer reports
     * every object still alive at vkDestroyDevice */
    uint8_t* f = malloc(W * H * 3);
    const float ts[2] = {1.0f / 3, 2.0f / 3};
    int ok = 1;
    for (int i = 0; i < 20 && ok; ++i) {
        te_context* ctx = make_ctx(dev, 2, 1, 2);
        ok = ctx != NULL;
        if (!ctx) break;
        for (int k = 0; k < 3; ++k) {
            pattern(f, 2.0 * k, 1.0 * k, 6);
            ok &= run_pair(ctx, (uint32_t)(k % 2), f, k ? 2 : 0, ts);
        }
        te_context_destroy(ctx);
    }
    EXPECT(ok, "20 context lifetimes with work");
    free(f);
}

static te_context* make_lmc(te_device* dev, uint32_t outputs);

static void test_timeline_protocol(te_device* dev, int lmc) {
    /* MAKO protocol: the producer signals idx when a real frame is ready;
     * output i signals idx + 1 + i; the next real frame is idx + N + 1.  The
     * first frame has no pair, so its outputs hold the new frame.  LMC: the
     * analysis is two submissions around the CPU motion fit, the protocol is
     * the same. */
    te_context* ctx = lmc ? make_lmc(dev, 2) : make_ctx(dev, 2, 1, 2);
    EXPECT(ctx, "context");
    if (!ctx) return;
    EXPECT(te_context_memory_bytes(ctx) > (uint64_t)W * H * 4 * 4, "context memory accounted (%llu bytes)",
           (unsigned long long)te_context_memory_bytes(ctx));
    uint8_t *a = malloc(W * H * 3), *b = malloc(W * H * 3), *o = malloc(W * H * 3);
    pattern(a, 0, 0, 9);
    pattern(b, 3, 1, 9);
    te_upload_rgb(ctx, 0, a);
    te_submit s0 = {.slot = 0, .count = 2, .timestamps = {1.0f / 3, 2.0f / 3}, .wait_value = 1, .signal_first = 2,
                    .download = 1};
    EXPECT(te_submit_frame(ctx, &s0) == VK_SUCCESS, "first frame with outputs");
    EXPECT(!te_context_idle(ctx, 20000000ull), "work waits for the producer's timeline value");
    EXPECT(te_timeline_value(ctx) == 0, "nothing signalled before the producer");
    te_timeline_signal(ctx, 1);
    EXPECT(te_context_idle(ctx, 5000000000ull), "work completes after the producer signal");
    EXPECT(te_timeline_value(ctx) == 3, "outputs signalled 2 and 3 (value %llu)", (unsigned long long)te_timeline_value(ctx));
    te_pair_stats st;
    te_last_stats(ctx, &st);
    EXPECT(st.held && st.outputs == 2, "first frame outputs are holds");
    te_download_rgb(ctx, 1, o);
    EXPECT(memcmp(o, a, W * H * 3) == 0, "hold output is the new real frame");
    te_upload_rgb(ctx, 1, b);
    te_timeline_signal(ctx, 4);
    te_submit s1 = {.slot = 1, .count = 2, .timestamps = {1.0f / 3, 2.0f / 3}, .wait_value = 4, .signal_first = 5};
    EXPECT(te_submit_frame(ctx, &s1) == VK_SUCCESS && te_context_idle(ctx, 5000000000ull), "pair");
    EXPECT(te_timeline_value(ctx) == 6, "pair outputs signalled 5 and 6 (value %llu)", (unsigned long long)te_timeline_value(ctx));
    te_last_stats(ctx, &st);
    EXPECT(!st.held, "second frame generates");
    /* a skipped frame: the same slot again is a discontinuity, outputs hold */
    te_upload_rgb(ctx, 1, a);
    te_timeline_signal(ctx, 7);
    te_submit s2 = {.slot = 1, .count = 1, .timestamps = {0.5f}, .wait_value = 7, .signal_first = 8, .discontinuity = 1,
                    .download = 1};
    EXPECT(te_submit_frame(ctx, &s2) == VK_SUCCESS && te_context_idle(ctx, 5000000000ull), "discontinuity accepted");
    te_last_stats(ctx, &st);
    te_download_rgb(ctx, 0, o);
    EXPECT(st.held && memcmp(o, a, W * H * 3) == 0, "discontinuity holds the new frame");
    EXPECT(te_timeline_value(ctx) == 8, "discontinuity output signalled");
    if (lmc) {
        /* a generated pair still in flight when the context goes: destroy waits for it (the fit
           worker releases the second half) instead of hanging */
        te_upload_rgb(ctx, 0, b);
        te_timeline_signal(ctx, 9);
        te_submit s3 = {.slot = 0, .count = 2, .timestamps = {1.0f / 3, 2.0f / 3}, .wait_value = 9, .signal_first = 10};
        EXPECT(te_submit_frame(ctx, &s3) == VK_SUCCESS, "LMC pair submitted before destroy");
    }
    free(a);
    free(b);
    free(o);
    te_context_destroy(ctx);
}

static int fd_closed(int fd) { return fcntl(fd, F_GETFD) == -1; }

static void test_fd_contract(void) {
    /* Two VkDevices like the layer and the MAKO backend: the producer exports
     * its source and destination images, the engine imports them. */
    te_device_desc dd = {.validation = 1};
    te_device *prod = NULL, *eng = NULL;
    EXPECT(te_device_create(&dd, &prod) == VK_SUCCESS, "producer device");
    EXPECT(te_device_create(&dd, &eng) == VK_SUCCESS, "engine device");
    if (!prod || !eng) goto out;
    int mem = 0, sem = 0;
    te_device_supports_fd_import(eng, &mem, &sem);
    printf("  fd import: memory=%d semaphore=%d on %s\n", mem, sem, te_device_name(eng));
    if (!mem) {
        printf("  SKIP memory fd import: VK_KHR_external_memory_fd missing\n");
        goto out;
    }
    te_context_desc pd;
    te_context_desc_default(&pd, W, H, 1);
    pd.host_io = 1;
    pd.export_fds = 1;
    te_context* producer = NULL;
    EXPECT(te_context_create(prod, &pd, &producer) == VK_SUCCESS, "exporting context");
    if (!producer) goto out;
    te_context_desc ed;
    te_context_desc_default(&ed, W, H, 1);
    ed.host_io = 1;
    for (int s = 0; s < 2; ++s) ed.source_fds[s] = te_export_source_fd(producer, (uint32_t)s);
    ed.dest_fds[0] = te_export_dest_fd(producer, 0);
    EXPECT(ed.source_fds[0] >= 0 && ed.source_fds[1] >= 0 && ed.dest_fds[0] >= 0, "exported fds");
    te_context* engine = NULL;
    EXPECT(te_context_create(eng, &ed, &engine) == VK_SUCCESS, "importing context");
    uint8_t *a = malloc(W * H * 3), *b = malloc(W * H * 3), *o = malloc(W * H * 3), *mid = malloc(W * H * 3);
    pattern(a, 0, 0, 8);
    pattern(b, 4, 2, 8);
    pattern(mid, 2, 1, 8);
    if (engine) {
        /* the producer fills its (shared) source images; the engine generates
         * into the shared destination, which the producer reads back */
        const float ts[1] = {0.5f};
        te_upload_rgb(producer, 0, a);
        te_submit s0 = {.slot = 0};
        te_submit_frame(producer, &s0);
        te_wait_idle(producer);
        te_submit e0 = {.slot = 0};
        EXPECT(te_submit_frame(engine, &e0) == VK_SUCCESS && te_wait_idle(engine) == VK_SUCCESS, "engine frame 0");
        te_upload_rgb(producer, 1, b);
        te_submit s1 = {.slot = 1};
        te_submit_frame(producer, &s1);
        te_wait_idle(producer);
        te_submit e1 = {.slot = 1, .count = 1, .timestamps = {ts[0]}, .download = 1};
        EXPECT(te_submit_frame(engine, &e1) == VK_SUCCESS && te_wait_idle(engine) == VK_SUCCESS, "engine pair");
        te_download_rgb(engine, 0, o);
        double q = psnr_rect(o, mid, 16, 16, W - 16, H - 16);
        EXPECT(q > 34.0, "generation through imported images: PSNR %.2f dB", q);
        te_context_destroy(engine);
    }
    if (!sem) {
        /* capability check: an unsupported sync fd is refused and closed */
        int fds[2];
        if (pipe(fds) == 0) {
            te_context_desc sd;
            te_context_desc_default(&sd, W, H, 1);
            sd.sync_fd = fds[0];
            te_context* bad = NULL;
            EXPECT(te_context_create(eng, &sd, &bad) != VK_SUCCESS && !bad, "sync fd refused without semaphore fd");
            EXPECT(fd_closed(fds[0]), "refused sync fd is closed (ownership taken)");
            close(fds[1]);
            printf("  semaphore fd import not supported here: refusal path tested, import path untested\n");
        }
    }
    free(a);
    free(b);
    free(o);
    free(mid);
    te_context_destroy(producer);
out:
    if (prod) EXPECT(te_device_validation_errors(prod) == 0, "producer validation errors");
    if (eng) EXPECT(te_device_validation_errors(eng) == 0, "engine validation errors");
    te_device_destroy(prod);
    te_device_destroy(eng);
}

/* ---------------------------------------------------------------- scheduler */

static te_sched_observation obs(double fixed, double per_frame, uint32_t outputs, uint32_t tiles, uint32_t light,
                                uint32_t full, int profiled) {
    te_sched_observation o;
    memset(&o, 0, sizeof o);
    o.timed = 1;
    o.profiled = profiled;
    o.outputs = outputs;
    o.tile_total = tiles;
    o.fixed_ms = fixed;
    o.classify_ms = 0.05;
    for (uint32_t i = 0; i < outputs; ++i) {
        o.lists_ms[i] = 0.05;
        o.reuse_ms[i] = 0.001 * (tiles - light - full);
        o.light_ms[i] = 0.01 * light;
        o.full_ms[i] = 0.1 * full;
        o.total_ms[i] = per_frame;
        o.tiles[i][0] = tiles - light - full;
        o.tiles[i][1] = light;
        o.tiles[i][2] = full;
    }
    return o;
}

static void test_scheduler(void) {
    te_scheduler s;
    te_scheduler_init(&s);
    EXPECT(s.cap_ms == TE_SCHED_DEFAULT_CAP_MS, "a cost cap is on by default");
    s.cap_ms = 0.0; /* budget-only behaviour first */
    te_sched_input in = {.real_interval_ms = 33.3, .gpu_busy = 0.5, .requested = 2};
    te_sched_decision d = te_scheduler_decide(&s, &in);
    EXPECT(d.action == TE_SCHED_GENERATE && d.reason == TE_REASON_WARMUP && d.profile, "warm-up generates and profiles");
    /* cheap pairs: stays at the best quality level */
    for (int i = 0; i < 10; ++i) {
        te_sched_observation o = obs(1.0, 2.0, 2, 100, 40, 20, i == 0);
        te_scheduler_observe(&s, &o, d.budget_ms);
        d = te_scheduler_decide(&s, &in);
    }
    EXPECT(d.action == TE_SCHED_GENERATE && d.level == 0 && d.outputs == 2, "cheap scene: level %u outputs %u", d.level,
           d.outputs);
    EXPECT(d.budget_ms > 14.0 && d.budget_ms < 14.3, "budget = interval * free share * safety (%.2f)", d.budget_ms);
    /* expensive pairs: misses push to cheaper levels, then shed outputs, then fall back */
    te_sched_input heavy = {.real_interval_ms = 33.3, .gpu_busy = 0.9, .requested = 2};
    int fell_back = 0, raised = 0;
    for (int i = 0; i < 40 && !fell_back; ++i) {
        d = te_scheduler_decide(&s, &heavy);
        raised |= d.level > 0;
        fell_back = d.action == TE_SCHED_FALLBACK;
        te_sched_observation o = obs(1.0, 3.0, 2, 100, 40, 60, 1);
        te_scheduler_observe(&s, &o, d.budget_ms);
    }
    EXPECT(raised, "over budget raises the level");
    EXPECT(fell_back && d.reason == TE_REASON_OVER_BUDGET, "unmeetable budget falls back (reason %d)", d.reason);
    /* adaptivity that does not pay is switched off */
    te_scheduler_init(&s);
    s.cap_ms = 0.0;
    for (int i = 0; i < 20; ++i) {
        te_sched_observation o = obs(1.0, 2.0, 2, 100, 0, 100, 1);
        te_scheduler_observe(&s, &o, 30.0);
    }
    te_sched_input roomy = {.real_interval_ms = 100.0, .gpu_busy = 0.2, .requested = 2};
    d = te_scheduler_decide(&s, &roomy);
    EXPECT(d.force_full && d.reason == TE_REASON_ADAPTIVE_NOT_WORTH,
           "no LIGHT/REUSE tiles: classification disabled (reason %d)", d.reason);
    for (int i = 0; i < 260; ++i) {
        te_sched_observation o = obs(1.0, 2.0, 2, 100, 0, 100, 0);
        te_scheduler_observe(&s, &o, 30.0);
    }
    d = te_scheduler_decide(&s, &roomy);
    EXPECT(!d.force_full, "classification is re-evaluated after a while");
    /* hard cap: over cap_ms per output climbs to LIGHT-only, then recommends fewer outputs */
    te_scheduler_init(&s);
    s.cap_ms = 2.0;
    te_sched_input roomy2 = {.real_interval_ms = 100.0, .gpu_busy = 0.0, .requested = 2};
    int light_only = 0;
    for (int i = 0; i < 12; ++i) {
        d = te_scheduler_decide(&s, &roomy2);
        light_only |= d.force_light;
        te_sched_observation o = obs(2.0, 3.0, 2, 100, 40, 60, 0);   /* 4 ms per output measured */
        te_scheduler_observe(&s, &o, d.budget_ms);
    }
    EXPECT(d.budget_ms <= 4.0 + 1e-9, "cap bounds the budget (%.2f)", d.budget_ms);
    EXPECT(light_only && d.level == TE_SCHED_LEVELS - 1, "over the cap reaches LIGHT-only (level %u)", d.level);
    EXPECT(d.outputs < 2, "still over the cap at LIGHT-only: fewer outputs recommended (%u)", d.outputs);
    /* recovery: calm pairs lower the level again */
    te_scheduler_init(&s);
    s.cap_ms = 0.0;
    s.level = 2;
    s.samples = 10;
    for (int i = 0; i < 31; ++i) {
        te_sched_observation o = obs(0.5, 1.0, 2, 100, 40, 20, 0);
        te_scheduler_observe(&s, &o, 30.0);
    }
    d = te_scheduler_decide(&s, &in);
    EXPECT(d.level == 1, "calm pairs improve quality one level (%u)", d.level);
}


/* ------------------------------------------------------------------ LMC path */

static te_context* make_lmc(te_device* dev, uint32_t outputs) {
    te_context_desc d;
    te_context_desc_default(&d, W, H, outputs);
    d.host_io = 1;
    const char* shared=getenv("GFG_TEST_SHARED_FLOW");
    d.params.algorithm = shared?TE_ALGO_SHARED_FLOW:getenv("GFG_TEST_GPU_LMC")?TE_ALGO_LMC_GPU:TE_ALGO_LMC;
    if(shared)d.shared_flow_dense=atoi(shared);
    te_context* ctx = NULL;
    if (te_context_create(dev, &d, &ctx) != VK_SUCCESS) return NULL;
    return ctx;
}

typedef struct { te_context* ctx; uint64_t value; } delayed_signal;

static void* signal_later(void* raw) {
    delayed_signal* s = raw;
    usleep(150000);
    te_timeline_signal(s->ctx, s->value);
    return NULL;
}

static double monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void test_lmc_readiness_deadline(te_device* dev) {
    te_context* ctx = make_lmc(dev, 2);
    EXPECT(ctx, "LMC bounded-wait context");
    if (!ctx) return;
    uint8_t* f = malloc(W * H * 3);
    pattern(f, 0, 0, 11);
    const float ts[2] = {1.0f / 3, 2.0f / 3};
    EXPECT(run_pair(ctx, 0, f, 0, ts), "LMC deadline history");
    te_upload_rgb(ctx, 1, f);
    te_submit s = {.slot = 1, .count = 2, .timestamps = {1.0f / 3, 2.0f / 3},
                   .wait_value = 2, .signal_first = 3};
    EXPECT(te_submit_frame(ctx, &s) == VK_SUCCESS, "analysis waiting for an unavailable source");
    delayed_signal signal = {ctx, 2};
    pthread_t thread;
    int threaded = pthread_create(&thread, NULL, signal_later, &signal) == 0;
    EXPECT(threaded, "deadline release thread");
    if (!threaded) te_timeline_signal(ctx, 2);
    double begin = monotonic_ms();
    int idle = te_context_idle(ctx, 5000000ull);
    double elapsed = monotonic_ms() - begin;
    if (threaded) {
        EXPECT(!idle && elapsed < 100.0, "5 ms readiness deadline returned in %.2f ms, idle=%d", elapsed, idle);
        te_submit next = {.slot = 0, .nonblocking = 1};
        begin = monotonic_ms();
        VkResult rc = te_submit_frame(ctx, &next);
        int gpu_model=getenv("GFG_TEST_GPU_LMC")!=NULL || getenv("GFG_TEST_SHARED_FLOW")!=NULL;
        EXPECT((rc==VK_NOT_READY || (gpu_model && rc==VK_SUCCESS)) && monotonic_ms()-begin<100.0,
               "nonblocking submit must not wait for unavailable GPU work or a fit worker (%d)",rc);
        pthread_join(thread, NULL);
    }
    EXPECT(te_context_idle(ctx, 5000000000ull), "same pair finishes after its source arrives");
    EXPECT(te_timeline_value(ctx) == 4, "outputs signal only after real work completes");
    free(f);
    te_context_destroy(ctx);
}

/* static overlay ("HUD") drawn over a frame */
static void overlay(uint8_t* rgb) {
    for (int y = 8; y < 28; ++y)
        for (int x = 8; x < 72; ++x) {
            uint8_t* p = rgb + 3 * (y * W + x);
            p[0] = (uint8_t)(x * 3);
            p[1] = 230;
            p[2] = (uint8_t)(y * 7);
        }
}

static void test_lmc_static(te_device* dev) {
    te_context* ctx = make_lmc(dev, 2);
    EXPECT(ctx, "LMC context");
    if (!ctx) return;
    uint8_t *a = malloc(W * H * 3), *o = malloc(W * H * 3);
    pattern(a, 0, 0, 11);
    const float ts[2] = {1.0f / 3, 2.0f / 3};
    EXPECT(run_pair(ctx, 0, a, 0, ts), "LMC first frame");
    EXPECT(run_pair(ctx, 1, a, 2, ts), "LMC static pair");
    for (uint32_t i = 0; i < 2; ++i) {
        te_download_rgb(ctx, i, o);
        double q = psnr_rect(o, a, 0, 0, W, H);
        EXPECT(q > 50.0, "LMC static scene output %u PSNR %.2f dB", i, q);
        if(getenv("GFG_TEST_SHARED_FLOW"))EXPECT(memcmp(o,a,W*H*3)==0,"GSF static pixels exact");
    }
    free(a);
    free(o);
    te_context_destroy(ctx);
}

static void test_lmc_pan(te_device* dev, int tier) {
    /* a camera pan: (6, 2) per real frame; after a few pairs the plane, the
     * palette and the temporal predictor are all in play.  Tier 1 takes the A
     * grid from the B grid, tier 2 also runs one propagation sweep. */
    g_lmc_tier = tier;
    te_context* ctx = make_lmc(dev, 2);
    EXPECT(ctx, "LMC context");
    if (!ctx) return;
    uint8_t *f = malloc(W * H * 3), *prev = malloc(W * H * 3), *mid = malloc(W * H * 3), *o = malloc(W * H * 3);
    const float ts[2] = {1.0f / 3, 2.0f / 3};
    double worst = 99.0, rep = 0.0;
    te_pair_stats st;
    memset(&st, 0, sizeof st);
    for (int k = 0; k < 5; ++k) {
        memcpy(prev, f, W * H * 3);
        pattern(f, 6.0 * k, 2.0 * k, 12);
        EXPECT(run_pair(ctx, (uint32_t)(k % 2), f, k ? 2 : 0, ts), "LMC pan pair %d", k);
        if (k < 3) continue;
        for (uint32_t i = 0; i < 2; ++i) {
            double t = ts[i];
            pattern(mid, 6.0 * (k - 1 + t), 2.0 * (k - 1 + t), 12);
            te_download_rgb(ctx, i, o);
            double q = psnr_rect(o, mid, 16, 16, W - 16, H - 16);
            double r = psnr_rect(t < 0.5 ? prev : f, mid, 16, 16, W - 16, H - 16);
            if (q < worst) worst = q;
            if (r > rep) rep = r;
        }
        te_last_stats(ctx, &st);
    }
    EXPECT(worst > 32.0 && worst > rep + 8.0, "LMC pan (tier %d) reconstruction worst %.2f dB (repeat best %.2f dB)",
           tier, worst, rep);
    EXPECT(st.lmc_tier == tier, "LMC pan: pair recorded at tier %d (%d)", tier, st.lmc_tier);
    g_lmc_tier = 0;
    /* RANSAC samples must lie 120 px apart: planes are a full-resolution
     * feature (bench/parity.py checks them at 1280x800); here only the bound */
    EXPECT(st.tiles[0][0] <= 3, "LMC pan: at most three motion planes (%u)", st.tiles[0][0]);
    EXPECT(st.timed, "LMC timestamps available");
    EXPECT(st.ready_ms[0] > 0 && st.ready_ms[1] >= st.ready_ms[0],
           "LMC cumulative output-ready timestamps %.3f / %.3f ms", st.ready_ms[0], st.ready_ms[1]);
    EXPECT(st.ready_ms[1] >= st.stage_ms[TE_STAGE_PYRAMID] + st.stage_ms[TE_STAGE_ANALYZE],
           "full pair time includes analysis and fit, not only composition");
    free(f);
    free(prev);
    free(mid);
    free(o);
    te_context_destroy(ctx);
}

static void colour_alias_pattern(uint8_t* rgb, double ox, double oy) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double u = x - ox, v = y - oy;
            uint8_t* p = rgb + 3 * (y * W + x);
            p[0] = clamp8(255 * (0.5 + 0.22 * sin(u * .17) * cos(v * .13)));
            p[1] = clamp8(255 * (0.5 + 0.21 * sin(v * .19 + u * .07)));
            p[2] = clamp8(255 * (0.5 + 0.24 * cos(u * .11 - v * .15)));
        }
}

static void test_lmc_chroma_alias(te_device* dev) {
    // Repeated colour textures have competing coarse luma matches. A small
    // fractional pan must not turn into unrelated long vectors and tearing.
    te_context* ctx = make_lmc(dev, 2);
    EXPECT(ctx, "chroma-alias context");
    if (!ctx) return;
    uint8_t *f = malloc(W * H * 3), *truth = malloc(W * H * 3), *o = malloc(W * H * 3);
    const float ts[2] = {1.0f / 3, 2.0f / 3};
    double worst = 99;
    for (int k = 0; k < 5; ++k) {
        colour_alias_pattern(f, 6 * k, 1.5 * k);
        EXPECT(run_pair(ctx, (uint32_t)(k % 2), f, k ? 2 : 0, ts), "chroma-alias pair %d", k);
        if (!k) continue;
        for (uint32_t i = 0; i < 2; ++i) {
            colour_alias_pattern(truth, 6 * (k - 1 + ts[i]), 1.5 * (k - 1 + ts[i]));
            te_download_rgb(ctx, i, o);
            double q = psnr_rect(o, truth, 16, 16, W - 16, H - 16);
            if (q < worst) worst = q;
        }
    }
    EXPECT(worst > 30.0, "coloured texture pan reconstructs instead of luma-alias tearing: %.2f dB", worst);
    free(f); free(truth); free(o);
    te_context_destroy(ctx);
}

static void test_lmc_hud(te_device* dev) {
    /* an overlay that stays still while the scene pans must come out exact
     * once the still counter has seen it for HUD_PAIRS pairs */
    te_context* ctx = make_lmc(dev, 2);
    EXPECT(ctx, "LMC context");
    if (!ctx) return;
    uint8_t *f = malloc(W * H * 3), *o = malloc(W * H * 3);
    const float ts[2] = {1.0f / 3, 2.0f / 3};
    for (int k = 0; k < 10; ++k) {
        pattern(f, 5.0 * k, 0.0, 13);
        overlay(f);
        EXPECT(run_pair(ctx, (uint32_t)(k % 2), f, k ? 2 : 0, ts), "LMC HUD pair %d", k);
    }
    int exact = 1;
    for (uint32_t i = 0; i < 2; ++i) {
        te_download_rgb(ctx, i, o);
        for (int y = 10; y < 26; ++y)
            for (int x = 10; x < 70; ++x)
                for (int c = 0; c < 3; ++c)
                    if (o[3 * (y * W + x) + c] != f[3 * (y * W + x) + c]) exact = 0;
    }
    EXPECT(exact, "LMC keeps a still overlay exact over a moving scene");
    free(f);
    free(o);
    te_context_destroy(ctx);
}

static void test_lmc_hold(te_device* dev) {
    te_context* ctx = make_lmc(dev, 1);
    EXPECT(ctx, "LMC context");
    if (!ctx) return;
    uint8_t *a = malloc(W * H * 3), *b = malloc(W * H * 3), *o = malloc(W * H * 3);
    pattern(a, 0, 0, 14);
    pattern(b, 9, 3, 14);
    const float ts[1] = {0.5f};
    EXPECT(run_pair(ctx, 0, a, 0, ts), "LMC first frame");
    EXPECT(te_upload_rgb(ctx, 1, b) == VK_SUCCESS, "upload");
    te_submit s = {.slot = 1, .count = 1, .download = 1, .hold = 1};
    s.timestamps[0] = 0.5f;
    EXPECT(te_submit_frame(ctx, &s) == VK_SUCCESS && te_wait_idle(ctx) == VK_SUCCESS, "LMC hold submit");
    te_download_rgb(ctx, 0, o);
    EXPECT(memcmp(o, b, W * H * 3) == 0, "LMC hold shows the newest real frame bit for bit");
    free(a);
    free(b);
    free(o);
    te_context_destroy(ctx);
}

static void test_shared_cut(te_device* dev) {
    if(!getenv("GFG_TEST_SHARED_FLOW"))return;
    te_context* ctx=make_lmc(dev,2);
    EXPECT(ctx,"GSF cut context");if(!ctx)return;
    uint8_t *a=malloc(W*H*3),*b=malloc(W*H*3),*o=malloc(W*H*3);
    noise(a,42);noise(b,912);
    const float ts[2]={1.0f/3,2.0f/3};
    EXPECT(run_pair(ctx,0,a,0,ts),"GSF cut history");
    EXPECT(run_pair(ctx,1,b,2,ts),"GSF cut pair");
    te_pair_stats st;te_last_stats(ctx,&st);
    EXPECT(st.scene_cut==1,"GSF unrelated noise is cut recovery");
    for(uint32_t i=0;i<2;++i){
        EXPECT(te_download_rgb(ctx,i,o)==VK_SUCCESS,"GSF cut output");
        EXPECT(memcmp(o,i?b:a,W*H*3)==0,"GSF cut copies endpoint, never counts as unique synthesis");
    }
    EXPECT(run_pair(ctx,0,b,2,ts),"GSF post-cut static pair");
    te_last_stats(ctx,&st);EXPECT(!st.scene_cut,"GSF cut state does not stick");
    EXPECT(st.source_duplicate,"GSF exact repeated RGB pair is a duplicate, never unique synthesis");
    for(uint32_t i=0;i<2;++i){te_download_rgb(ctx,i,o);EXPECT(memcmp(o,b,W*H*3)==0,"GSF post-cut pixels exact");}
    free(a);free(b);free(o);te_context_destroy(ctx);
}

int main(void) {
    test_scheduler();
    te_device_desc dd = {.validation = 1};
    te_device* dev = NULL;
    if (te_device_create(&dd, &dev) != VK_SUCCESS) {
        fprintf(stderr, "no Vulkan device: %s\n", te_last_error());
        return 2;
    }
    printf("te-tests on %s\n", te_device_name(dev));
    test_static_scene(dev);
    test_translation(dev);
    test_scene_cut(dev);
    test_forced_modes(dev);
    test_hysteresis(dev);
    test_invalid_inputs(dev);
    test_lifetime(dev);
    test_timeline_protocol(dev, 0);
    // Warm the software driver's lazy LMC shader compilation before testing
    // bounded protocol waits; compilation time is not a semaphore deadline.
    test_lmc_static(dev);
    test_timeline_protocol(dev, 1);
    test_lmc_readiness_deadline(dev);
    for (int tier = 0; tier < TE_LMC_TIERS; ++tier) test_lmc_pan(dev, tier);
    test_lmc_chroma_alias(dev);
    test_lmc_hud(dev);
    test_lmc_hold(dev);
    test_shared_cut(dev);
    uint32_t verr = te_device_validation_errors(dev);
    te_device_destroy(dev);
    EXPECT(verr == 0, "validation errors: %u", verr);
    test_fd_contract();
    printf("te-tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Non-Deck comparison harness. Compile this SAME file against each pinned engine.
// Reuse the actual test host, resource resolver, scene and production pass plans.
// display_copy_proxy is explicitly NOT Gamescope BLIT or a KMS/scanout test.
#define _POSIX_C_SOURCE 200809L
#define main kfg_original_correctness_main
#include "kfg_vktest.c"
#undef main
#include "kfg_post.h"
#include "kfg_smaa_tex.h"
#include <time.h>

#ifndef KFG_COMPARE_CANDIDATE
#define KFG_COMPARE_CANDIDATE 0
#endif

typedef struct compare_env {
    engine base;
    kfg_vk_image post[2], display, edges, weights, area, search;
} compare_env;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const kfg_vk_image *compare_resolve(void *user, kfg_image_ref ref)
{
    compare_env *e = user;
    switch (ref.role) {
    case KFG_ROLE_POST_IN: return &e->base.output;
    case KFG_ROLE_POST: return ref.ring == 2 ? &e->display : &e->post[ref.ring & 1];
    case KFG_ROLE_SMAA_EDGES: return &e->edges;
    case KFG_ROLE_SMAA_WEIGHTS: return &e->weights;
    case KFG_ROLE_SMAA_AREA: return &e->area;
    case KFG_ROLE_SMAA_SEARCH: return &e->search;
    default: return resolve(&e->base, ref);
    }
}

// Same compute shader, constants, nearest sampling and dispatch as RecordCopy().
// Deliberately not vkCmdCopyImage: that was only a pixel-parity test in posttest.
static void append_copy(kfg_plan *p, kfg_image_ref src, kfg_image_ref dst, uint32_t w, uint32_t h)
{
    kfg_pass *x = kfg_plan_add(p, KFG_SHADER_COPY, dst, "identity copy");
    x->slots[0] = src;
    x->nslots = 1;
    x->nearest_mask = 1;
    x->params.src_size[0] = (int32_t)w;
    x->params.src_size[1] = (int32_t)h;
    uint32_t size[2] = { w, h };
    kfg_set_domain(x, size, 8, 8);
}

static int compare_init(compare_env *e, kfg_vk *vk, uint32_t w, uint32_t h)
{
    memset(e, 0, sizeof(*e));
    if (engine_init(&e->base, vk, w, h, VK_FORMAT_R8G8B8A8_UNORM)) return -1;
    kfg_vk_image *images[] = { &e->post[0], &e->post[1], &e->display, &e->edges, &e->weights };
    for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); ++i)
        if (kfg_vk_image_create(vk, images[i], VK_FORMAT_R8G8B8A8_UNORM, w, h)) return -1;
    if (kfg_vk_image_create(vk, &e->area, VK_FORMAT_R8G8B8A8_UNORM, KFG_SMAA_AREA_W, KFG_SMAA_AREA_H) ||
        kfg_vk_image_create(vk, &e->search, VK_FORMAT_R8_UNORM, KFG_SMAA_SEARCH_W, KFG_SMAA_SEARCH_H)) return -1;
    uint8_t *area = malloc((size_t)KFG_SMAA_AREA_W * KFG_SMAA_AREA_H * 4);
    if (!area) return -1;
    kfg_smaa_area_rgba8(area);
    int rc = kfg_vk_upload_raw(vk, &e->area, area, (size_t)KFG_SMAA_AREA_W * KFG_SMAA_AREA_H * 4);
    free(area);
    return rc || kfg_vk_upload_raw(vk, &e->search, kfg_smaa_search_r8(), (size_t)KFG_SMAA_SEARCH_W * KFG_SMAA_SEARCH_H);
}

static void compare_destroy(compare_env *e)
{
    kfg_vk_image *images[] = { &e->post[0], &e->post[1], &e->display, &e->edges, &e->weights, &e->area, &e->search };
    for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); ++i)
        kfg_vk_image_destroy(e->base.vk, images[i]);
    engine_destroy(&e->base);
}

static int compare_plan(kfg_plan *p, const char *name, const char *spec,
                        uint32_t w, uint32_t h, int optimized, float phase)
{
    kfg_tuning tuning = kfg_default_tuning();
    if (!strcmp(name, "analysis")) {
        kfg_plan_analyze(p, w, h, 0, 1, 1, 1, -1, &tuning);
        return 0;
    }
    kfg_plan_generate(p, w, h, 0, 1, 1, phase, 0);
    if (spec[0]) {
        kfg_post_cfg cfg;
        char error[128];
        kfg_plan post;
        if (kfg_post_parse(spec, &cfg, error, sizeof(error))) return -1;
        int ring = kfg_plan_post(&post, w, h, &cfg, 0.375f);
        if (ring < 0) return -1;
        if (optimized) {
#if KFG_COMPARE_CANDIDATE
            if (!kfg_plan_post_into(&post, kfg_ref(KFG_ROLE_COLOR, 2, 0))) return -1;
#else
            return -1;
#endif
        }
        if (p->n + post.n + 1 >= KFG_MAX_PASSES) return -1;
        memcpy(p->pass + p->n, post.pass, post.n * sizeof(kfg_pass));
        p->n += post.n;
        if (!optimized)
            append_copy(p, kfg_ref(KFG_ROLE_POST, (uint32_t)ring, 0), kfg_ref(KFG_ROLE_COLOR, 2, 0), w, h);
    }
    if (!strcmp(name, "display_copy_proxy") && !optimized)
        append_copy(p, kfg_ref(KFG_ROLE_OUTPUT, 0, 0), kfg_ref(KFG_ROLE_POST, 2, 0), w, h);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 9) {
        fprintf(stderr, "usage: compare SPV legacy|optimized W H SAMPLES WARMUP OUTPUT_DIR LABEL\n");
        return 2;
    }
    int optimized = !strcmp(argv[2], "optimized");
    uint32_t w = (uint32_t)atoi(argv[3]), h = (uint32_t)atoi(argv[4]);
    int samples = atoi(argv[5]), warmup = atoi(argv[6]);
    if ((optimized && !KFG_COMPARE_CANDIDATE) || w < 32 || h < 32 || samples < 3 || warmup < 3) return 2;
    // argc includes the binary; argv[8] is reserved run label for raw provenance.
    const char *outdir = argv[7];
    kfg_vk vk;
    if (kfg_vk_init(&vk, argv[1], false) || !vk.timestamps) return 1;
    compare_env e;
    if (compare_init(&e, &vk, w, h)) return 1;
    char path[2048];
    snprintf(path, sizeof(path), "%s/meta.txt", outdir);
    FILE *meta = fopen(path, "w");
    if (!meta) return 1;
    fprintf(meta, "device=%s\ndriver=%s\ndriver_id=%u\nlabel=%s\n", vk.device_name, vk.driver_info, vk.driver_id, argv[8]);
    fclose(meta);
    size_t bytes = (size_t)w * h * 4;
    uint8_t *pixels = malloc(bytes);
    if (!pixels) return 1;
    g_geo = (double)w / 1280.0;
    kfg_tuning tuning = kfg_default_tuning();
    for (int f = 0; f < 2; ++f) {
        render(&SCENES[2], (double)f, (int)w, (int)h, pixels);
        if (kfg_vk_upload_rgba8(&vk, &e.base.color[f], pixels)) return 1;
        kfg_plan plan;
        kfg_plan_analyze(&plan, w, h, 0, (uint32_t)f, (uint32_t)f, f > 0, -1, &tuning);
        if (run_plan(&e.base, &plan, NULL, NULL)) return 1;
    }
    const struct { const char *name, *spec; } cases[] = {
        { "analysis", "" }, { "warp_x2", "" }, { "warp_x3", "" },
        { "sharp", "cas=0.5" }, { "vivid", "vibrance,cas=0.35" },
        { "smooth", "cas=0.3,smaa" }, { "display_copy_proxy", "" },
    };
    snprintf(path, sizeof(path), "%s/samples.csv", outdir);
    FILE *csv = fopen(path, "w");
    if (!csv) return 1;
    fprintf(csv, "case,sample,phase,passes,vulkan_elapsed_ns,pass_sum_ns,record_ns,submit_wait_ns,wall_ns\n");
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        for (int i = -warmup; i < samples + 3; ++i) {
            // Measurement includes x2 and both x3 phases; readback is outside timing.
            int phase_index = (i + warmup) % 3;
            float phase = !strcmp(cases[c].name, "warp_x2") ? 0.5f : phase_index == 0 ? 0.5f : (float)phase_index / 3.0f;
            kfg_plan p;
            if (compare_plan(&p, cases[c].name, cases[c].spec, w, h, optimized, phase)) return 1;
            uint64_t begin = mono_ns();
            kfg_vk_begin(&vk);
            if (kfg_vk_record(&vk, &p, compare_resolve, &e)) return 1;
            uint64_t recorded = mono_ns();
            if (kfg_vk_submit_wait(&vk)) return 1;
            uint64_t done = mono_ns();
            double times[KFG_MAX_PASSES], sum = 0;
            uint32_t count = kfg_vk_pass_times(&vk, times, KFG_MAX_PASSES);
            if (count != p.n) return 1;
            for (uint32_t j = 0; j < count; ++j) sum += times[j];
            uint64_t timestamps[256];
            if (vkGetQueryPoolResults(vk.device, vk.queries, 0, vk.query_used, sizeof(timestamps), timestamps,
                    sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return 1;
            double elapsed = (double)(timestamps[vk.query_used - 1] - timestamps[0]) * vk.timestamp_period_ns;
            if (i >= 0 && i < samples)
                fprintf(csv, "%s,%d,%.6f,%u,%.0f,%.0f,%llu,%llu,%llu\n", cases[c].name, i, phase, p.n, elapsed, sum,
                    (unsigned long long)(recorded - begin), (unsigned long long)(done - recorded), (unsigned long long)(done - begin));
            if (i >= samples && strcmp(cases[c].name, "analysis")) {
                const kfg_vk_image *output = cases[c].spec[0] ? &e.base.color[2] :
                    !strcmp(cases[c].name, "display_copy_proxy") && !optimized ? &e.display : &e.base.output;
                if (kfg_vk_download_rgba8(&vk, output, pixels)) return 1;
                snprintf(path, sizeof(path), "%s/%s-phase%d.rgba", outdir, cases[c].name, phase_index);
                FILE *image = fopen(path, "wb");
                if (!image || fwrite(pixels, 1, bytes, image) != bytes) return 1;
                fclose(image);
            }
        }
    }
    fclose(csv);
    free(pixels);
    compare_destroy(&e);
    kfg_vk_destroy(&vk);
    return 0;
}

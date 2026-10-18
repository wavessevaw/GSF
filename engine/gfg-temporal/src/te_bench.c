/* te-bench: run the Temporal Engine on recorded real frames.
 *
 * Reads an N x H x W x 3 uint8 .npy sequence, keeps every K-th frame as the
 * "real" input, generates the K-1 frames in between on the GPU and writes them
 * to an .npy for comparison with the frames that were held out.  GPU time per
 * stage comes from Vulkan timestamp queries.
 */
#define _GNU_SOURCE
#include "te_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    void* map;
    const uint8_t* data;
    size_t frames, height, width;
    size_t map_size;
} npy_frames;

static int npy_open(const char* path, npy_frames* out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    void* map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return -1;
    const uint8_t* m = map;
    if (st.st_size < 16 || memcmp(m, "\x93NUMPY", 6) != 0) return -2;
    size_t hlen, off;
    if (m[6] == 1) {
        hlen = (size_t)m[8] | ((size_t)m[9] << 8);
        off = 10;
    } else {
        hlen = (size_t)m[8] | ((size_t)m[9] << 8) | ((size_t)m[10] << 16) | ((size_t)m[11] << 24);
        off = 12;
    }
    char header[1024];
    if (hlen >= sizeof header) return -2;
    memcpy(header, m + off, hlen);
    header[hlen] = 0;
    if (!strstr(header, "'|u1'") || strstr(header, "'fortran_order': True")) return -3;
    const char* shape = strstr(header, "'shape': (");
    unsigned long n, h, w, c;
    if (!shape || sscanf(shape, "'shape': (%lu, %lu, %lu, %lu)", &n, &h, &w, &c) != 4 || c != 3) return -3;
    out->map = map;
    out->data = m + off + hlen;
    out->frames = n;
    out->height = h;
    out->width = w;
    out->map_size = (size_t)st.st_size;
    return 0;
}

static FILE* npy_create(const char* path, size_t n, size_t h, size_t w) {
    FILE* f = fopen(path, "wb");
    if (!f) return NULL;
    char dict[256];
    int len = snprintf(dict, sizeof dict, "{'descr': '|u1', 'fortran_order': False, 'shape': (%zu, %zu, %zu, 3), }", n, h, w);
    int total = 10 + len + 1;
    int pad = (64 - total % 64) % 64;
    unsigned short hl = (unsigned short)(len + pad + 1);
    fwrite("\x93NUMPY\x01\x00", 1, 8, f);
    fputc(hl & 0xff, f);
    fputc(hl >> 8, f);
    fwrite(dict, 1, (size_t)len, f);
    for (int i = 0; i < pad; ++i) fputc(' ', f);
    fputc('\n', f);
    return f;
}

static void usage(void) {
    fprintf(stderr,
            "usage: te-bench --frames seq.npy [--start S] [--pairs N] [--step K] [--outputs N] [--crop WxH]\n"
            "                [--algo tiles|lmc|lmc-gpu|gsf|shared-flow|shared-flow-dense|shared-flow-affine|shared-flow-palette]\n"
            "                [--tier 0..2] [--mode adaptive|full|light|reuse] [--tile 8|16|32] [--levels L] [--range R]\n"
            "                [--light-thr X] [--probe N] [--profile] [--out gen.npy] [--json stats.jsonl]\n"
            "                [--analysis DIR] [--device NAME] [--validation] [--no-download]\n");
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char** argv) {
    const char *frames_path = NULL, *out_path = NULL, *json_path = NULL, *analysis_dir = NULL, *device = NULL;
    size_t start = 0, pairs = 0, step = 3, cw = 0, ch = 0;
    int profile = 0, validation = 0, download = 1, tier = 0;
    uint32_t outputs=0;int shared_dense=0;
    te_params params;
    te_params_default(&params);
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (strcmp(a, name) == 0 && v && (++i, 1))
        if (ARG("--frames")) frames_path = v;
        else if (ARG("--start")) start = strtoul(v, NULL, 10);
        else if (ARG("--outputs")) outputs=(uint32_t)strtoul(v,NULL,10);
        else if (ARG("--pairs")) pairs = strtoul(v, NULL, 10);
        else if (ARG("--step")) step = strtoul(v, NULL, 10);
        else if (ARG("--crop")) {
            if (sscanf(v, "%zux%zu", &cw, &ch) != 2) { usage(); return 2; }
        } else if (ARG("--mode")) {
            params.adaptive = strcmp(v, "adaptive") == 0;
            if (!params.adaptive) {
                if (strcmp(v, "full") == 0) params.force_class = 2;
                else if (strcmp(v, "light") == 0) params.force_class = 1;
                else if (strcmp(v, "reuse") == 0) params.force_class = 0;
                else { usage(); return 2; }
            }
        } else if (ARG("--algo")) {
            if (strcmp(v,"gsf")==0){params.algorithm=TE_ALGO_SHARED_FLOW;shared_dense=4;}
            else if (strcmp(v,"shared-flow-palette")==0){params.algorithm=TE_ALGO_SHARED_FLOW;shared_dense=3;}
            else if (strcmp(v,"shared-flow-affine")==0){params.algorithm=TE_ALGO_SHARED_FLOW;shared_dense=2;}
            else if (strcmp(v,"shared-flow-dense")==0){params.algorithm=TE_ALGO_SHARED_FLOW;shared_dense=1;}
            else if (strcmp(v,"shared-flow")==0) params.algorithm=TE_ALGO_SHARED_FLOW;
            else if (strcmp(v,"lmc-gpu")==0) params.algorithm=TE_ALGO_LMC_GPU;
            else if (strcmp(v, "lmc") == 0) params.algorithm = TE_ALGO_LMC;
            else if (strcmp(v, "tiles") == 0) params.algorithm = TE_ALGO_TILES;
            else { usage(); return 2; }
        } else if (ARG("--tile")) params.tile = (uint32_t)strtoul(v, NULL, 10);
        else if (ARG("--levels")) params.levels = (uint32_t)strtoul(v, NULL, 10);
        else if (ARG("--range")) params.coarse_range = (int32_t)strtol(v, NULL, 10);
        else if (ARG("--light-thr")) params.light_thr = strtof(v, NULL);
        else if (ARG("--probe")) params.probe = (uint32_t)strtoul(v, NULL, 10);
        else if (ARG("--tier")) tier = (int)strtol(v, NULL, 10);
        else if (ARG("--out")) out_path = v;
        else if (ARG("--json")) json_path = v;
        else if (ARG("--analysis")) analysis_dir = v;
        else if (ARG("--device")) device = v;
        else if (strcmp(a, "--profile") == 0) profile = 1;
        else if (strcmp(a, "--validation") == 0) validation = 1;
        else if (strcmp(a, "--no-download") == 0) download = 0;
        else { usage(); return 2; }
#undef ARG
    }
    if (!frames_path || step < 2 || step - 1 > TE_MAX_OUTPUTS) { usage(); return 2; }
    if(!outputs)outputs=(uint32_t)(step-1);
    if(outputs>step-1){usage();return 2;}
    npy_frames seq;
    int rc = npy_open(frames_path, &seq);
    if (rc) {
        fprintf(stderr, "te-bench: cannot read %s as an N x H x W x 3 uint8 .npy (%d)\n", frames_path, rc);
        return 1;
    }
    size_t W = cw ? cw : seq.width, H = ch ? ch : seq.height;
    if (W > seq.width || H > seq.height) { fprintf(stderr, "te-bench: crop exceeds frame\n"); return 2; }
    size_t avail = seq.frames > start ? (seq.frames - 1 - start) / step : 0;
    if (!pairs || pairs > avail) pairs = avail;
    if (!pairs) { fprintf(stderr, "te-bench: not enough frames\n"); return 2; }
    te_device_desc dd = {.device_name = device, .validation = validation};
    te_device* dev;
    if (te_device_create(&dd, &dev) != VK_SUCCESS) {
        fprintf(stderr, "te-bench: device: %s\n", te_last_error());
        return 3;
    }
    te_context_desc cd;
    te_context_desc_default(&cd, (uint32_t)W, (uint32_t)H, outputs);
    cd.params = params;cd.shared_flow_dense=shared_dense;
    cd.profile = profile;
    cd.host_io = 1;
    te_context* ctx;
    if (te_context_create(dev, &cd, &ctx) != VK_SUCCESS) {
        fprintf(stderr, "te-bench: context: %s\n", te_last_error());
        te_device_destroy(dev);
        return 3;
    }
    uint32_t tx, ty;
    uint32_t tiles = te_tile_count(ctx, &tx, &ty);
    fprintf(stderr, "te-bench: %s %zux%zu algo %s tiles %ux%u pairs %zu step %zu mode %s%s\n", te_device_name(dev), W, H,
            params.algorithm == TE_ALGO_SHARED_FLOW ? (shared_dense==4?"gsf":shared_dense==3?"shared-flow-palette":shared_dense==2?"shared-flow-affine":shared_dense?"shared-flow-dense":"shared-flow") : params.algorithm == TE_ALGO_LMC_GPU ? "lmc-gpu" : params.algorithm == TE_ALGO_LMC ? "lmc" : "tiles", tx, ty, pairs, step, params.adaptive ? "adaptive" : (params.force_class == 2 ? "full" : params.force_class == 1 ? "light" : "reuse"),
            profile ? " profile" : "");
    FILE* out = out_path ? npy_create(out_path, pairs * outputs, H, W) : NULL;
    FILE* json = json_path ? fopen(json_path, "w") : NULL;
    FILE *fvec = NULL, *fcls = NULL, *fstat = NULL;
    if (analysis_dir) {
        char p[4096];
        mkdir(analysis_dir, 0755);
        snprintf(p, sizeof p, "%s/vectors.f32", analysis_dir);
        fvec = fopen(p, "wb");
        snprintf(p, sizeof p, "%s/classes.u8", analysis_dir);
        fcls = fopen(p, "wb");
        snprintf(p, sizeof p, "%s/stats.f32", analysis_dir);
        fstat = fopen(p, "wb");
        snprintf(p, sizeof p, "%s/meta.json", analysis_dir);
        FILE* m = fopen(p, "w");
        if (m) {
            fprintf(m, "{\"tiles_x\": %u, \"tiles_y\": %u, \"pairs\": %zu, \"outputs\": %u}\n", tx, ty, pairs, outputs);
            fclose(m);
        }
    }
    uint8_t* rgb = malloc(W * H * 3);
    uint8_t* gen = malloc(W * H * 3);
    float* vec = malloc((size_t)tiles * 8);
    float* st8 = malloc((size_t)tiles * 32);
    uint8_t* cls = malloc(tiles);
    int status = 0;
    double wall = 0.0;
    for (size_t k = 0; k <= pairs && !status; ++k) {
        size_t idx = start + k * step;
        const uint8_t* frame = seq.data + idx * seq.height * seq.width * 3;
        for (size_t y = 0; y < H; ++y) memcpy(rgb + y * W * 3, frame + y * seq.width * 3, W * 3);
        uint32_t slot = (uint32_t)(k % 2);
        if (te_upload_rgb(ctx, slot, rgb) != VK_SUCCESS) { status = 4; break; }
        te_submit s = {.slot = slot, .count = k ? outputs : 0, .download = download, .lmc_tier = tier};
        for (size_t j = 1; j <= outputs; ++j) s.timestamps[j - 1] = (float)j / (float)step;
        double t0 = now_ms();
        if (te_submit_frame(ctx, &s) != VK_SUCCESS) { status = 4; break; }
        double submit_ms=now_ms()-t0;
        if(te_wait_idle(ctx)!=VK_SUCCESS){status=4;break;}
        double cpu_total_ms=now_ms()-t0;
        if (k) wall += now_ms() - t0;
        if (!k) continue;
        te_pair_stats ps;
        if (te_last_stats(ctx, &ps) != VK_SUCCESS) { status = 4; break; }
        if (out && download) {
            for (uint32_t i = 0; i < ps.outputs; ++i) {
                te_download_rgb(ctx, i, gen);
                fwrite(gen, 1, W * H * 3, out);
            }
        }
        if (analysis_dir) {
            if (te_read_vectors(ctx, vec) != VK_SUCCESS || te_read_classes(ctx, cls) != VK_SUCCESS ||
                te_read_stats(ctx, st8) != VK_SUCCESS) { status = 4; break; }
            fwrite(vec, 8, tiles, fvec);
            fwrite(cls, 1, tiles, fcls);
            fwrite(st8, 32, tiles, fstat);
        }
        if (json) {
            fprintf(json,"{\"algorithm\":%u,\"shared_flow_variant\":%d,\"model_fallback\":%d,\"cut_bad_blocks\":%u,\"cut_total_blocks\":%u,\"outputs\":%u,\"allocation_bytes\":%llu,\"cpu_submit_ms\":%.4f,\"cpu_total_ms\":%.4f,\"analysis_ms\":%.4f,",
                params.algorithm,shared_dense,ps.shared_flow_fallback,ps.shared_flow_cut_bad_blocks,
                (unsigned)((W/8)*(H/8)),ps.outputs,(unsigned long long)te_context_memory_bytes(ctx),submit_ms,cpu_total_ms,
                ps.stage_ms[0]+ps.stage_ms[1]+ps.stage_ms[2]);
            fprintf(json, "\"pair\": %llu, \"a\": %zu, \"b\": %zu, \"timed\": %d, \"profile\": %d, \"pyramid_ms\": %.4f, "
                          "\"analyze_ms\": %.4f, \"classify_ms\": %.4f, \"scene_cut\": %u, \"promoted\": %u, "
                          "\"confidence\": %.4f, \"tiles\": %u",
                    (unsigned long long)ps.pair, idx - step, idx, ps.timed, profile, ps.stage_ms[TE_STAGE_PYRAMID],
                    ps.stage_ms[TE_STAGE_ANALYZE], ps.stage_ms[TE_STAGE_CLASSIFY], ps.scene_cut, ps.promoted,
                    ps.confidence, ps.tile_total);
            if (ps.lmc_ms[0] >= 0.0) {
                static const char* names[TE_LMC_PROF_COUNT] = {"search_b", "search_a", "layers", "fit_wait",
                                                               "field", "masked", "orphan", "field2"};
                fprintf(json, ", \"lmc_ms\": {");
                for (int k = 0; k < TE_LMC_PROF_COUNT; ++k)
                    fprintf(json, "%s\"%s\": %.4f", k ? ", " : "", ps.shared_flow && k==TE_LMC_FIT_WAIT?"pair_control":ps.model_gpu && k==TE_LMC_FIT_WAIT?"gpu_model":names[k], ps.lmc_ms[k]);
                fprintf(json, "}, \"cpu_fit_ms\": %.4f, \"first_ready_ms\": %.4f, \"pair_ready_ms\": %.4f, "
                              "\"lmc_counts\": [%u, %u, %u]", ps.lmc_fit_ms, ps.ready_ms[0],
                        ps.ready_ms[ps.outputs - 1], ps.tiles[0][0], ps.tiles[0][1], ps.tiles[0][2]);
            }
            fprintf(json, ", \"frames\": [");
            for (uint32_t i = 0; i < ps.outputs; ++i)
                fprintf(json, "%s{\"t\": %.6f, \"lists_ms\": %.4f, \"reuse_ms\": %.4f, \"light_ms\": %.4f, "
                              "\"full_ms\": %.4f, \"total_ms\": %.4f, \"reuse\": %u, \"light\": %u, \"full\": %u}",
                        i ? ", " : "", s.timestamps[i], ps.frame_ms[i][TE_FRAME_LISTS], ps.frame_ms[i][TE_FRAME_REUSE],
                        ps.frame_ms[i][TE_FRAME_LIGHT], ps.frame_ms[i][TE_FRAME_FULL], ps.frame_ms[i][TE_FRAME_TOTAL],
                        ps.tiles[i][0], ps.tiles[i][1], ps.tiles[i][2]);
            fprintf(json, "]}\n");
        }
    }
    if (status) fprintf(stderr, "te-bench: %s\n", te_last_error());
    uint32_t verrors = te_device_validation_errors(dev);
    fprintf(stderr, "te-bench: done, wall %.1f ms for %zu pairs, validation errors %u\n", wall, pairs, verrors);
    if (out) fclose(out);
    if (json) fclose(json);
    if (fvec) fclose(fvec);
    if (fcls) fclose(fcls);
    if (fstat) fclose(fstat);
    free(rgb);
    free(gen);
    free(vec);
    free(st8);
    free(cls);
    te_context_destroy(ctx);
    te_device_destroy(dev);
    munmap(seq.map, seq.map_size);
    if (!status && validation && verrors) status = 5;
    return status;
}

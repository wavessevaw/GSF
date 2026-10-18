/* GFG Temporal Engine, LMC path (layered motion compensation).
 *
 * Included by te_engine.c: it shares the Vulkan loader, the helpers and the
 * context's sources, destinations, timeline, command buffers and queries;
 * only the analysis and the per-output passes differ from the tile path.
 * Every pass mirrors bench/gen_lmc2.py (see shaders/lmc/lmc_common.glsl). */

#define LMC_LEVELS 4
#define LMC_BLK 4
#define LMC_MAX_LAYERS 3
#define LMC_PALETTE 4
#define LMC_HYPS 128
#define LMC_BINDINGS 39

enum {
    LMC_PREP, LMC_DOWN, LMC_SEARCH, LMC_LAYERS, LMC_FIELD, LMC_MASKED, LMC_ORPHAN, LMC_SELECT,
    LMC_COMPOSE, LMC_GPU_HYP, LMC_GPU_IRLS, LMC_GPU_PALETTE, LMC_SF_PREPARE, LMC_SF_COMPOSE, LMC_SF_MODEL, LMC_PIPES
};

typedef struct {
    int32_t size[4];
    int32_t lsize[5][4];
    int32_t blk_off[4];
    int32_t counts[4];
    float pen_update, smooth, margin, tol_px;
    float still, orphan_resid, orphan_share, orphan_max_cost;
    int32_t coarse_r, propagate, hud_pairs, orphan_r;
    float min_layer_share;
    int32_t ransac_iters, orphan_cap, pad1;
} lmc_ubo;
_Static_assert(offsetof(lmc_ubo, blk_off) == 96, "std140 layout of LmcParams");
_Static_assert(offsetof(lmc_ubo, pen_update) == 128, "std140 layout of LmcParams");
_Static_assert(sizeof(lmc_ubo) == 192, "std140 size of LmcParams");

typedef struct {
    float t;
    int32_t level, grid, pass, parity, flags;
} lmc_push;

/* byte offsets inside the Layers buffer (std430) */
#define LMC_LY_LAYERS 16u
#define LMC_LY_PREV (LMC_LY_LAYERS + 48u * LMC_MAX_LAYERS)
#define LMC_LY_HYPS (LMC_LY_PREV + 48u * LMC_MAX_LAYERS)
#define LMC_LY_INLIERS (LMC_LY_HYPS + 48u * (LMC_HYPS + LMC_MAX_LAYERS))
#define LMC_LY_LIVE (LMC_LY_INLIERS + 4u * (LMC_HYPS + LMC_MAX_LAYERS))
/* Globals buffer */
#define LMC_G_ORPHANS 80u
#define LMC_G_OHIST (96u + 4u * 64u * 64u)
#define LMC_ORPHAN_MAX 1024u
#define LMC_ORPHAN_CAP 256u                                  /* default; GFG_LMC_ORPHAN_CAP */
#define LMC_G_OCLEAR (4u * (2u * 65u + 2u + 2u))              /* histogram, thresholds, list lengths */
#define LMC_G_SIZE (LMC_G_OHIST + LMC_G_OCLEAR + 4u * 2u * LMC_ORPHAN_MAX)
#define LMC_FIT_SUB 2   /* CPU fit on every 2nd level-1 block in each direction */

struct te_lmc {
    uint32_t w2, h2, n1, nblk;
    uint32_t ocap;              /* orphan blocks searched per grid */
    uint32_t lw[LMC_LEVELS + 1], lh[LMC_LEVELS + 1], bw[LMC_LEVELS + 1], bh[LMC_LEVELS + 1];
    uint32_t off[LMC_LEVELS + 1];
    te_image luma[2][LMC_LEVELS + 1], half[2], hud, field[2], linear[2];
    te_buffer params, vec[2], scratch, temporal, layers, globals, orph[2], masked, sel;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set0[2], set1[TE_MAX_OUTPUTS];
    VkPipeline pipe[LMC_PIPES];
    VkPipeline search[LMC_LEVELS + 1][2];  /* block search specialised per level (1..4) and grid */
    int have_temporal;
    int gpu_fit;
    int shared_flow;
    int compact_flow;
    unsigned fit_sync_ok;
    /* motion model on the CPU (te_lmc_fit.c), fitted on this pair's vectors: the analysis is two
       submissions.  The first (pyramid, block search, block statistics) ends with a copy of the
       level-1 B vectors, usable flags and motion votes to fitrb and signals fit_tl = 2k + 1; the
       worker waits for it, fits planes and palette, writes them to stage, then submits the second
       half (copies the model in, builds the fields) and the outputs.  Nothing on the queue waits
       for the host, and the engine has its own device and queue: the game's work never waits. */
    te_device* dev;
    te_buffer fitrb[TE_RING];
    te_buffer stage[TE_RING];   /* host-visible model for the second submission (LMC_STAGE_*) */
    VkSemaphore fit_tl;
    uint64_t fit_seq;           /* generated pairs submitted (k) */
    te_lmc_fit_work* fit_work;
    pthread_t fit_thread;
    int fit_thread_ok;
    pthread_mutex_t fit_lock;
    pthread_cond_t fit_cond;
    pthread_cond_t fit_done;    /* a job finished */
    int fit_quit, fit_busy;
    struct { te_slot* slot; int rs; uint64_t seq; uint32_t gen; } jobs[TE_RING];
    int job_head, job_count;
    float* job_vec;
    unsigned char *job_usable, *job_hudb;
    uint32_t fit_pairs;
    uint32_t fit_gen;           /* a skip or a hold bumps it: no RANSAC hypotheses from before */
    te_lmc_model model_prev;
    int prev_valid;
};

#define LMC_FITRB_SIZE(n1) ((VkDeviceSize)(n1) * 20u + 16u)

static double lmc_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* stage layout: what the second submission copies into the Layers and Globals buffers */
#define LMC_STAGE_COUNT 0u      /* uint layer_count -> Layers 0 */
#define LMC_STAGE_LAYERS 16u    /* Layer[3] (12 floats, share in [11]) -> Layers LMC_LY_LAYERS */
#define LMC_STAGE_HEAD 256u     /* uint pal_count, moving -> Globals 0 */
#define LMC_STAGE_PAL 272u      /* vec4[4] -> Globals 16 */
#define LMC_STAGE_SIZE 512u

/* one fit: pair seq k of ring slot rs (its first submission signals 2k + 1) */
static void lmc_fit_job(struct te_lmc* m, te_slot* slot, int rs, uint64_t k, uint32_t gen) {
    te_device* dev = m->dev;
    select_device(dev);
    uint64_t want = 2 * k + 1;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &m->fit_tl, .pValues = &want};
    VkResult wr;
    do wr = vkWaitSemaphores(dev->device, &wi, 100000000ull);   /* 100 ms steps */
    while (wr == VK_TIMEOUT);
    double t0 = lmc_now_ms();
    uint8_t* st = m->stage[rs].map;
    memset(st, 0, LMC_STAGE_SIZE);
    if (wr == VK_SUCCESS) {
        const uint8_t* rb = m->fitrb[rs].map;
        uint32_t n = m->n1;
        const float* v4 = (const float*)rb;
        const uint32_t* live = (const uint32_t*)(rb + (size_t)n * 16u);
        for (uint32_t i = 0; i < n; ++i) {
            m->job_vec[2 * i] = v4[4 * i] * 2.0f;          /* level-1 px -> full-res px */
            m->job_vec[2 * i + 1] = v4[4 * i + 1] * 2.0f;
            m->job_hudb[i] = v4[4 * i + 3] > 0.5f;
            m->job_usable[i] = live[i] != 0;
        }
        uint32_t votes;
        memcpy(&votes, rb + (size_t)n * 20u, 4);
        pthread_mutex_lock(&m->fit_lock);
        int have_prev = m->prev_valid && gen == m->fit_gen;
        te_lmc_model prev = m->model_prev;
        pthread_mutex_unlock(&m->fit_lock);
        te_lmc_model md;
        te_lmc_fit(m->fit_work, m->job_vec, m->job_usable, m->job_hudb, (int)m->bw[1], (int)m->bh[1], LMC_FIT_SUB,
                   ++m->fit_pairs, have_prev ? &prev : NULL, &md);
        uint32_t count = md.nlayers < LMC_MAX_LAYERS ? md.nlayers : LMC_MAX_LAYERS;
        memcpy(st + LMC_STAGE_COUNT, &count, 4);
        float* L = (float*)(st + LMC_STAGE_LAYERS);
        for (uint32_t j = 0; j < count; ++j) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) L[12 * j + r * 4 + c] = md.layers[j].h[r][c];
            L[12 * j + 11] = md.layers[j].share;
        }
        /* the HUD counter of the next pair advances when this pair moved */
        uint32_t head[2] = {md.npal < LMC_PALETTE ? md.npal : LMC_PALETTE, votes * 2u > n ? 1u : 0u};
        memcpy(st + LMC_STAGE_HEAD, head, sizeof head);
        float* P = (float*)(st + LMC_STAGE_PAL);
        for (uint32_t j = 0; j < head[0]; ++j) {
            P[4 * j] = md.pal[j][0];
            P[4 * j + 1] = md.pal[j][1];
        }
        pthread_mutex_lock(&m->fit_lock);
        if (gen == m->fit_gen) {
            m->model_prev = md;
            m->prev_valid = 1;
        }
        slot->lmc_fit_ms = lmc_now_ms() - t0;
        pthread_mutex_unlock(&m->fit_lock);
    }
    /* A cleanup fence is not proof of completed outputs. Preserve the error
       for wait_slot; never collect unwritten queries or count fake work. */
    VkResult submitted = queue_submit(dev, slot->lnsub, slot->lsub, slot->fence);
    if (submitted != VK_SUCCESS)
        queue_submit(dev, 0, NULL, slot->fence);
    pthread_mutex_lock(&m->fit_lock);
    slot->async_result = submitted;
    slot->lmc_pending = 0;
    pthread_mutex_unlock(&m->fit_lock);
}

static void* lmc_fit_main(void* arg) {
    struct te_lmc* m = arg;
    pthread_mutex_lock(&m->fit_lock);
    for (;;) {
        /* the queue is drained before quitting: a submitted second half waits for its signal */
        while (!m->job_count && !m->fit_quit) pthread_cond_wait(&m->fit_cond, &m->fit_lock);
        if (!m->job_count) break;
        te_slot* slot = m->jobs[m->job_head].slot;
        int rs = m->jobs[m->job_head].rs;
        uint64_t k = m->jobs[m->job_head].seq;
        uint32_t gen = m->jobs[m->job_head].gen;
        m->job_head = (m->job_head + 1) % TE_RING;
        m->job_count--;
        m->fit_busy = 1;
        pthread_mutex_unlock(&m->fit_lock);
        lmc_fit_job(m, slot, rs, k, gen);
        pthread_mutex_lock(&m->fit_lock);
        m->fit_busy = 0;
        pthread_cond_broadcast(&m->fit_done);
    }
    pthread_mutex_unlock(&m->fit_lock);
    return NULL;
}

/* a generated pair was submitted: queue its fit */
static void lmc_fit_enqueue(te_context* ctx, te_slot* slot) {
    struct te_lmc* m = ctx->lmc;
    pthread_mutex_lock(&m->fit_lock);
    int t = (m->job_head + m->job_count) % TE_RING;
    slot->lmc_pending = 1;
    m->jobs[t].slot = slot;
    m->jobs[t].rs = (int)(slot - ctx->ring);
    m->jobs[t].seq = slot->lmc_seq;
    m->jobs[t].gen = m->fit_gen;
    m->job_count++;
    pthread_cond_signal(&m->fit_cond);
    pthread_mutex_unlock(&m->fit_lock);
}

static int lmc_slot_pending(te_context* ctx, te_slot* slot) {
    struct te_lmc* m = ctx->lmc;
    pthread_mutex_lock(&m->fit_lock);
    int p = slot->lmc_pending;
    pthread_mutex_unlock(&m->fit_lock);
    return p;
}

/* until every queued fit has run and submitted its second half */
static void lmc_drain(te_context* ctx) {
    struct te_lmc* m = ctx->lmc;
    if (!m->fit_thread_ok) return;
    pthread_mutex_lock(&m->fit_lock);
    while (m->job_count || m->fit_busy) pthread_cond_wait(&m->fit_done, &m->fit_lock);
    pthread_mutex_unlock(&m->fit_lock);
}

/* Unlike teardown's drain, a readiness probe must honour the caller's
 * deadline even if the GPU has not signalled the first analysis submission. */
static int lmc_wait_pending(te_context* ctx, te_slot* slot, uint64_t timeout_ns) {
    struct te_lmc* m = ctx->lmc;
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    if (timeout_ns != UINT64_MAX) {
        until.tv_sec += (time_t)(timeout_ns / 1000000000ull);
        until.tv_nsec += (long)(timeout_ns % 1000000000ull);
        if (until.tv_nsec >= 1000000000l) { until.tv_sec++; until.tv_nsec -= 1000000000l; }
    }
    pthread_mutex_lock(&m->fit_lock);
    int ok = 1;
    while (slot->lmc_pending) {
        if (!timeout_ns) { ok = 0; break; }
        int rc = timeout_ns == UINT64_MAX ? pthread_cond_wait(&m->fit_done, &m->fit_lock)
            : pthread_cond_timedwait(&m->fit_done, &m->fit_lock, &until);
        if (rc) { ok = 0; break; }
    }
    pthread_mutex_unlock(&m->fit_lock);
    return ok;
}

static void lmc_push_c(te_context* ctx, VkCommandBuffer cb, float t, int level, int grid, int pass, int flags) {
    lmc_push pc = {.t = t, .level = level, .grid = grid, .pass = pass, .parity = 0, .flags = flags};
    vkCmdPushConstants(cb, ctx->lmc->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
}

static VkResult lmc_create(te_context* ctx) {
    te_device* dev = ctx->dev;
    VkDevice d = dev->device;
    struct te_lmc* m = calloc(1, sizeof *m);
    if (!m) return fail(VK_ERROR_OUT_OF_HOST_MEMORY, "out of memory");
    ctx->lmc = m;
    m->shared_flow=ctx->desc.params.algorithm==TE_ALGO_SHARED_FLOW;
    m->compact_flow=m->shared_flow && !ctx->desc.shared_flow_dense;
    m->gpu_fit=ctx->desc.params.algorithm==TE_ALGO_LMC_GPU || m->shared_flow;
    if(pthread_mutex_init(&m->fit_lock,NULL)) return VK_ERROR_INITIALIZATION_FAILED;
    m->fit_sync_ok|=1;
    if(pthread_cond_init(&m->fit_cond,NULL)) return VK_ERROR_INITIALIZATION_FAILED;
    m->fit_sync_ok|=2;
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    int cond_error=pthread_cond_init(&m->fit_done,&attr);
    pthread_condattr_destroy(&attr);
    if(cond_error) return VK_ERROR_INITIALIZATION_FAILED;
    m->fit_sync_ok|=4;
    uint32_t W = ctx->desc.width, H = ctx->desc.height;
    m->w2 = W / 2;
    m->h2 = H / 2;
    m->lw[0] = W;
    m->lh[0] = H;
    for (int l = 1; l <= LMC_LEVELS; ++l) {   /* reference down(): odd edge dropped */
        m->lw[l] = m->lw[l - 1] / 2;
        m->lh[l] = m->lh[l - 1] / 2;
    }
    m->nblk = 0;
    for (int l = 1; l <= LMC_LEVELS; ++l) {
        m->bw[l] = m->lw[l] / LMC_BLK;
        m->bh[l] = m->lh[l] / LMC_BLK;
        m->off[l] = m->nblk;
        m->nblk += m->bw[l] * m->bh[l];
    }
    m->n1 = m->bw[1] * m->bh[1];
    if (m->bw[LMC_LEVELS] == 0 || m->bh[LMC_LEVELS] == 0)
        return fail(VK_ERROR_INITIALIZATION_FAILED, "frame too small for the LMC pyramid");
    VkResult r;
#define LTRY(x) do { r = (x); if (r != VK_SUCCESS) return r; } while (0)
    const VkImageUsageFlags su = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int s = 0; s < 2; ++s) {
        for (int l = 0; l <= LMC_LEVELS; ++l)
            LTRY(image_create(dev, &m->luma[s][l], m->lw[l], m->lh[l], VK_FORMAT_R32_SFLOAT, su, -1, 0));
        LTRY(image_create(dev, &m->half[s], m->w2, m->h2, VK_FORMAT_R16G16B16A16_SFLOAT, su, -1, 0));
        LTRY(image_create(dev, &m->field[s], m->compact_flow?1:m->w2, m->compact_flow?1:m->h2, VK_FORMAT_R16G16B16A16_SFLOAT, su, -1, 0));
        if(ctx->desc.color_transfer==TE_TRANSFER_PQ)
            LTRY(image_create(dev,&m->linear[s],W,H,VK_FORMAT_R16G16B16A16_SFLOAT,su,-1,0));
    }
    LTRY(image_create(dev, &m->hud, W, H, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      -1, 0));
    const VkBufferUsageFlags bu = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    LTRY(buffer_create(dev, &m->params, sizeof(lmc_ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1));
    for (int g = 0; g < 2; ++g) {
        LTRY(buffer_create(dev, &m->vec[g], (VkDeviceSize)m->nblk * 16, bu, 0));
        LTRY(buffer_create(dev, &m->orph[g], (VkDeviceSize)m->n1 * 16, bu, 0));
    }
    LTRY(buffer_create(dev, &m->scratch, (VkDeviceSize)m->nblk * 16, bu, 0));
    LTRY(buffer_create(dev, &m->temporal, (VkDeviceSize)m->n1 * 16, bu, 0));
    /* live flags, ordered list, 3 counters, dense list (4 words per entry, 16-byte aligned start) */
    LTRY(buffer_create(dev, &m->layers, LMC_LY_LIVE + (VkDeviceSize)(((2 * m->n1 + 6) & ~3u) + 4 * m->n1) * 4, bu, 0));
    LTRY(buffer_create(dev, &m->globals, LMC_G_SIZE, bu, 0));
    LTRY(buffer_create(dev, &m->masked, m->compact_flow?16:(VkDeviceSize)(W / 4) * (H / 4) * 16, bu, 0));
    LTRY(buffer_create(dev, &m->sel, m->compact_flow?16:(VkDeviceSize)m->w2 * m->h2 * 16, bu, 0));

    lmc_ubo u;
    memset(&u, 0, sizeof u);
    u.size[0] = (int32_t)W; u.size[1] = (int32_t)H; u.size[2] = (int32_t)m->w2; u.size[3] = (int32_t)m->h2;
    for (int l = 0; l <= LMC_LEVELS; ++l) {
        u.lsize[l][0] = (int32_t)m->lw[l];
        u.lsize[l][1] = (int32_t)m->lh[l];
        u.lsize[l][2] = (int32_t)m->bw[l];
        u.lsize[l][3] = (int32_t)m->bh[l];
    }
    for (int l = 1; l <= LMC_LEVELS; ++l) u.blk_off[l - 1] = (int32_t)m->off[l];
    u.counts[0] = (int32_t)m->nblk; u.counts[1] = (int32_t)m->n1;
    u.counts[2] = (int32_t)(m->w2 * m->h2); u.counts[3] = (int32_t)(W * H);
    u.pen_update = 0.004f; u.smooth = 0.002f; u.margin = 0.06f; u.tol_px = 0.75f;
    u.still = 2.0f / 255.0f; u.orphan_resid = 0.15f; u.orphan_share = 0.08f; u.orphan_max_cost = 0.08f;
    u.coarse_r = 10; u.propagate = 2; u.hud_pairs = 6; u.orphan_r = 112;
    u.pad1=ctx->desc.output_bgra;
    u.min_layer_share = 0.10f; u.ransac_iters = LMC_HYPS;
    m->ocap = LMC_ORPHAN_CAP;
    const char* cap = getenv("GFG_LMC_ORPHAN_CAP");
    if (cap && *cap) {
        long v = strtol(cap, NULL, 10);
        m->ocap = v < 0 ? 0u : v > (long)LMC_ORPHAN_MAX ? LMC_ORPHAN_MAX : (uint32_t)v;
    }
    u.orphan_cap = (int32_t)m->ocap;
    memcpy(m->params.map, &u, sizeof u);

    /* descriptor layout: see lmc_common.glsl / lmc_store.glsl */
    const uint32_t bindings=ctx->desc.color_transfer==TE_TRANSFER_PQ?39u:36u;
    VkDescriptorSetLayoutBinding b[LMC_BINDINGS];
    for (uint32_t i = 0; i < bindings; ++i) {
        VkDescriptorType t = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (i == 0) t = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (i <= 12 || i == 18 || i == 19 || i == 28 || i == 29 || i==36 || i==37) t = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if ((i >= 13 && i <= 17) || i == 20 || i == 21 || i == 34 || i == 35 || i==38) t = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = t, .descriptorCount = 1,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    }
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = bindings, .pBindings = b};
    CHECK(vkCreateDescriptorSetLayout(d, &li, NULL, &m->dsl), "vkCreateDescriptorSetLayout(lmc)");
    VkDescriptorSetLayout sets[2] = {m->dsl, ctx->dsl1};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lmc_push)};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2,
                                      .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    CHECK(vkCreatePipelineLayout(d, &pli, NULL, &m->layout), "vkCreatePipelineLayout(lmc)");
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 18},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 12},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 10 + TE_MAX_OUTPUTS}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 2 + TE_MAX_OUTPUTS, .poolSizeCount = 4, .pPoolSizes = ps};
    CHECK(vkCreateDescriptorPool(d, &dpi, NULL, &m->pool), "vkCreateDescriptorPool(lmc)");
    VkDescriptorSetLayout l0[2] = {m->dsl, m->dsl};
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = m->pool, .descriptorSetCount = 2, .pSetLayouts = l0};
    CHECK(vkAllocateDescriptorSets(d, &ai, m->set0), "vkAllocateDescriptorSets(lmc)");
    VkDescriptorSetLayout l1[TE_MAX_OUTPUTS] = {ctx->dsl1, ctx->dsl1, ctx->dsl1, ctx->dsl1};
    ai.descriptorSetCount = ctx->desc.outputs;
    ai.pSetLayouts = l1;
    CHECK(vkAllocateDescriptorSets(d, &ai, m->set1), "vkAllocateDescriptorSets(lmc dst)");

    for (uint32_t par = 0; par < 2; ++par) {
        uint32_t a = par ^ 1u, bb = par;
        VkDescriptorImageInfo im[LMC_BINDINGS];
        VkDescriptorBufferInfo bf[LMC_BINDINGS];
        VkWriteDescriptorSet w[LMC_BINDINGS];
        memset(im, 0, sizeof im);
        memset(bf, 0, sizeof bf);
#define SAMP(i, view) im[i] = (VkDescriptorImageInfo){ctx->sampler, (view), VK_IMAGE_LAYOUT_GENERAL}
#define STOR(i, view) im[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, (view), VK_IMAGE_LAYOUT_GENERAL}
#define BUF(i, b_) bf[i] = (VkDescriptorBufferInfo){(b_).buf, 0, VK_WHOLE_SIZE}
        BUF(0, m->params);
        SAMP(1, ctx->src[a].view);
        SAMP(2, ctx->src[bb].view);
        for (int l = 0; l <= LMC_LEVELS; ++l) {
            SAMP(3 + l, m->luma[a][l].view);
            SAMP(8 + l, m->luma[bb][l].view);
            STOR(13 + l, m->luma[bb][l].view);
        }
        SAMP(18, m->half[a].view);
        SAMP(19, m->half[bb].view);
        STOR(20, m->half[bb].view);
        STOR(21, m->hud.view);
        BUF(22, m->vec[0]);
        BUF(23, m->vec[1]);
        BUF(24, m->scratch);
        BUF(25, m->temporal);
        BUF(26, m->layers);
        BUF(27, m->globals);
        SAMP(28, m->field[0].view);
        SAMP(29, m->field[1].view);
        BUF(30, m->orph[0]);
        BUF(31, m->orph[1]);
        BUF(32, m->masked);
        BUF(33, m->sel);
        STOR(34, m->field[0].view);
        STOR(35, m->field[1].view);
        if(ctx->desc.color_transfer==TE_TRANSFER_PQ) {
            SAMP(36,m->linear[a].view);SAMP(37,m->linear[bb].view);STOR(38,m->linear[bb].view);
        }
#undef SAMP
#undef STOR
#undef BUF
        for (uint32_t i = 0; i < bindings; ++i) {
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m->set0[par],
                                          .dstBinding = i, .descriptorCount = 1, .descriptorType = b[i].descriptorType};
            if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                b[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                w[i].pBufferInfo = &bf[i];
            else
                w[i].pImageInfo = &im[i];
        }
        vkUpdateDescriptorSets(d, bindings, w, 0, NULL);
    }
    for (uint32_t i = 0; i < ctx->desc.outputs; ++i) {
        VkDescriptorImageInfo di = {VK_NULL_HANDLE, ctx->dst[i].view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = m->set1[i],
                                  .dstBinding = 0, .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &di};
        vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
    }

    struct { const uint32_t* code; size_t size; } src[LMC_PIPES] = {
        [LMC_PREP] = {ctx->desc.color_transfer==TE_TRANSFER_PQ?te_spv_lmc_prep_pq:te_spv_lmc_prep,
                       ctx->desc.color_transfer==TE_TRANSFER_PQ?sizeof te_spv_lmc_prep_pq:sizeof te_spv_lmc_prep},
        [LMC_DOWN] = {te_spv_lmc_down, sizeof te_spv_lmc_down},
        [LMC_SEARCH] = {te_spv_lmc_search, sizeof te_spv_lmc_search},
        [LMC_LAYERS] = {te_spv_lmc_layers, sizeof te_spv_lmc_layers},
        [LMC_FIELD] = {te_spv_lmc_field, sizeof te_spv_lmc_field},
        [LMC_MASKED] = {te_spv_lmc_masked, sizeof te_spv_lmc_masked},
        [LMC_ORPHAN] = {te_spv_lmc_orphan, sizeof te_spv_lmc_orphan},
        [LMC_SELECT] = {te_spv_lmc_select, sizeof te_spv_lmc_select},
        [LMC_COMPOSE] = {ctx->desc.output_fp16 ? te_spv_lmc_compose_fp16 : te_spv_lmc_compose,
                         ctx->desc.output_fp16 ? sizeof te_spv_lmc_compose_fp16 : sizeof te_spv_lmc_compose},
        [LMC_SF_PREPARE] = {te_spv_lmc_sf_prepare, sizeof te_spv_lmc_sf_prepare},
        [LMC_SF_MODEL] = {te_spv_lmc_sf_model, sizeof te_spv_lmc_sf_model},
        [LMC_SF_COMPOSE] = {ctx->desc.output_fp16?te_spv_lmc_sf_compose_fp16:te_spv_lmc_sf_compose,
                           ctx->desc.output_fp16?sizeof te_spv_lmc_sf_compose_fp16:sizeof te_spv_lmc_sf_compose},
        [LMC_GPU_HYP] = {te_spv_lmc_hyp, sizeof te_spv_lmc_hyp},
        [LMC_GPU_IRLS] = {te_spv_lmc_irls, sizeof te_spv_lmc_irls},
        [LMC_GPU_PALETTE] = {te_spv_lmc_palette, sizeof te_spv_lmc_palette},
    };
    if(ctx->desc.color_transfer==TE_TRANSFER_PQ) {
        src[LMC_COMPOSE].code=te_spv_lmc_compose_pq;src[LMC_COMPOSE].size=sizeof te_spv_lmc_compose_pq;
        src[LMC_SF_COMPOSE].code=te_spv_lmc_sf_compose_pq;src[LMC_SF_COMPOSE].size=sizeof te_spv_lmc_sf_compose_pq;
    }
    for (int i = 0; i < LMC_PIPES; ++i) {
        if(i==LMC_SF_MODEL && ctx->desc.shared_flow_dense<2)continue;
        if(i==LMC_SF_COMPOSE && !m->compact_flow)continue;
        if(i>=LMC_SF_PREPARE && !m->shared_flow)continue;
        if(m->shared_flow && i>=LMC_GPU_HYP && i<LMC_SF_PREPARE && ctx->desc.shared_flow_dense!=4 && !(i==LMC_GPU_PALETTE && ctx->desc.shared_flow_dense==3))continue;
        if(m->compact_flow && i!=LMC_PREP && i!=LMC_DOWN && i!=LMC_SEARCH && i<LMC_SF_PREPARE)continue;
        if (i >= LMC_GPU_HYP && i<LMC_SF_PREPARE && !m->gpu_fit) continue;
        if (i == LMC_SEARCH) continue;  /* specialised per level and grid below */
        VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                         .codeSize = src[i].size, .pCode = src[i].code};
        VkShaderModule mod;
        CHECK(vkCreateShaderModule(d, &smci, NULL, &mod), "vkCreateShaderModule(lmc)");
        VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod,
                                                      .pName = "main"},
                                            .layout = m->layout};
        r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpci, NULL, &m->pipe[i]);
        vkDestroyShaderModule(d, mod, NULL);
        if (r != VK_SUCCESS) return fail(r, "vkCreateComputePipelines(lmc %d) failed (%d)", i, r);
    }
    /* block search: level and grid are specialization constants (lmc_search.comp) */
    {
        VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                         .codeSize = sizeof te_spv_lmc_search, .pCode = te_spv_lmc_search};
        VkShaderModule mod;
        CHECK(vkCreateShaderModule(d, &smci, NULL, &mod), "vkCreateShaderModule(lmc search)");
        for (int l = 1; l <= LMC_LEVELS && r == VK_SUCCESS; ++l)
            for (int g = 0; g < 2 && r == VK_SUCCESS; ++g) {
                int32_t spec[2] = {l, g};
                VkSpecializationMapEntry e[2] = {{0, 0, 4}, {1, 4, 4}};
                VkSpecializationInfo si = {.mapEntryCount = 2, .pMapEntries = e, .dataSize = sizeof spec, .pData = spec};
                VkComputePipelineCreateInfo cpci = {
                    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                              .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main",
                              .pSpecializationInfo = &si},
                    .layout = m->layout};
                r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpci, NULL, &m->search[l][g]);
            }
        vkDestroyShaderModule(d, mod, NULL);
        if (r != VK_SUCCESS) return fail(r, "vkCreateComputePipelines(lmc search) failed (%d)", r);
    }
    if(!m->gpu_fit){
    for (int k = 0; k < TE_RING; ++k) {
        LTRY(buffer_create(dev, &m->fitrb[k], LMC_FITRB_SIZE(m->n1), VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1));
        LTRY(buffer_create(dev, &m->stage[k], LMC_STAGE_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1));
    }
    m->dev = dev;
    VkSemaphoreTypeCreateInfo stci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &stci};
    LTRY(vkCreateSemaphore(d, &sci, NULL, &m->fit_tl));
    m->fit_work = te_lmc_fit_alloc();
    m->job_vec = malloc(sizeof(float) * 2u * m->n1);
    m->job_usable = malloc(m->n1);
    m->job_hudb = malloc(m->n1);
    if (!m->fit_work || !m->job_vec || !m->job_usable || !m->job_hudb)
        return fail(VK_ERROR_OUT_OF_HOST_MEMORY, "out of memory (lmc fit)");
    m->fit_thread_ok = pthread_create(&m->fit_thread, NULL, lmc_fit_main, m) == 0;
    if (!m->fit_thread_ok) return fail(VK_ERROR_INITIALIZATION_FAILED, "cannot start the LMC fit thread");
    }
#undef LTRY
    return VK_SUCCESS;
}

static void lmc_destroy(te_context* ctx) {
    struct te_lmc* m = ctx->lmc;
    if (!m) return;
    te_device* dev = ctx->dev;
    VkDevice d = dev->device;
    if (m->fit_thread_ok) {
        /* the device is idle: every queued fit has run (the worker drains its queue) */
        pthread_mutex_lock(&m->fit_lock);
        m->fit_quit = 1;
        pthread_cond_signal(&m->fit_cond);
        pthread_mutex_unlock(&m->fit_lock);
        pthread_join(m->fit_thread, NULL);
    }
    if(m->fit_sync_ok&1) pthread_mutex_destroy(&m->fit_lock);
    if(m->fit_sync_ok&2) pthread_cond_destroy(&m->fit_cond);
    if(m->fit_sync_ok&4) pthread_cond_destroy(&m->fit_done);
    if (m->fit_tl) vkDestroySemaphore(d, m->fit_tl, NULL);
    te_lmc_fit_free(m->fit_work);
    free(m->job_vec);
    free(m->job_usable);
    free(m->job_hudb);
    for (int r = 0; r < TE_RING; ++r) {
        buffer_destroy(dev, &m->fitrb[r]);
        buffer_destroy(dev, &m->stage[r]);
    }
    for (int i = 0; i < LMC_PIPES; ++i)
        if (m->pipe[i]) vkDestroyPipeline(d, m->pipe[i], NULL);
    for (int l = 0; l <= LMC_LEVELS; ++l)
        for (int g = 0; g < 2; ++g)
            if (m->search[l][g]) vkDestroyPipeline(d, m->search[l][g], NULL);
    if (m->pool) vkDestroyDescriptorPool(d, m->pool, NULL);
    if (m->layout) vkDestroyPipelineLayout(d, m->layout, NULL);
    if (m->dsl) vkDestroyDescriptorSetLayout(d, m->dsl, NULL);
    for (int s = 0; s < 2; ++s) {
        for (int l = 0; l <= LMC_LEVELS; ++l) image_destroy(dev, &m->luma[s][l]);
        image_destroy(dev, &m->half[s]);
        image_destroy(dev, &m->field[s]);
        image_destroy(dev, &m->linear[s]);
        buffer_destroy(dev, &m->vec[s]);
        buffer_destroy(dev, &m->orph[s]);
    }
    image_destroy(dev, &m->hud);
    buffer_destroy(dev, &m->params);
    buffer_destroy(dev, &m->scratch);
    buffer_destroy(dev, &m->temporal);
    buffer_destroy(dev, &m->layers);
    buffer_destroy(dev, &m->globals);
    buffer_destroy(dev, &m->masked);
    buffer_destroy(dev, &m->sel);
    free(m);
    ctx->lmc = NULL;
}

static void lmc_record_init(te_context* ctx, VkCommandBuffer cb) {
    struct te_lmc* m = ctx->lmc;
    te_image* all[2 * (LMC_LEVELS + 1) + 7];
    uint32_t n = 0;
    for (int s = 0; s < 2; ++s) {
        for (int l = 0; l <= LMC_LEVELS; ++l) all[n++] = &m->luma[s][l];
        all[n++] = &m->half[s];
        all[n++] = &m->field[s];
        if(m->linear[s].img)all[n++] = &m->linear[s];
    }
    all[n++] = &m->hud;
    VkImageMemoryBarrier b[2 * (LMC_LEVELS + 1) + 7];
    for (uint32_t i = 0; i < n; ++i)
        b[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = all[i]->img,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, n, b);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (uint32_t i = 0; i < n; ++i) vkCmdClearColorImage(cb, all[i]->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    te_buffer* zb[] = {&m->vec[0], &m->vec[1], &m->scratch, &m->temporal, &m->layers, &m->globals,
                       &m->orph[0], &m->orph[1], &m->masked, &m->sel};
    for (size_t i = 0; i < sizeof zb / sizeof zb[0]; ++i) vkCmdFillBuffer(cb, zb[i]->buf, 0, VK_WHOLE_SIZE, 0);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

static void lmc_fill(VkCommandBuffer cb, te_buffer* b, VkDeviceSize off, VkDeviceSize size, uint32_t v) {
    vkCmdFillBuffer(cb, b->buf, off, size, v);
}

static void lmc_transfer_barrier(VkCommandBuffer cb) {
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

static void lmc_search_grid(te_context* ctx, VkCommandBuffer cb, int grid, int temporal_ok, int sweeps) {
    struct te_lmc* m = ctx->lmc;
    /* per level: candidates, PROPAGATE sweeps, then the vector median (with its cost); the passes
       ping-pong between the grid buffer and the scratch buffer and the median writes the grid
       buffer, so nothing is copied.  Two sweeps: -> S, S -> G, G -> S, S -> G; one: -> G, G -> S,
       S -> G. */
    enum { RD_S = 2, WR_S = 4 };
    static const int pass2[4] = {0, 1, 1, 2}, io2[4] = {WR_S, RD_S, WR_S, RD_S};
    static const int pass1[3] = {0, 1, 2}, io1[3] = {0, WR_S, RD_S};
    const int* pass = sweeps == 2 ? pass2 : pass1;
    const int* io = sweeps == 2 ? io2 : io1;
    int n = sweeps == 2 ? 4 : 3;
    for (int l = LMC_LEVELS; l >= 1; --l) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->search[l][grid]);
        uint32_t g = groups(m->bw[l] * m->bh[l], 64);
        int t = (grid == 1 || temporal_ok) ? 1 : 0;
        for (int k = 0; k < n; ++k) {
            lmc_push_c(ctx, cb, 0.0f, l, grid, pass[k], t | io[k] | (m->compact_flow?16:m->shared_flow?32:0));
            vkCmdDispatch(cb, g, 1, 1);
            compute_barrier(cb);
        }
    }
}

/* Single-grid tier: the level-1 A grid from the B grid instead of a second pyramid search
   (reference: gen_lmc2.estimate_a_from_b).  Carry every B vector to where its content was in A
   (the cheapest wins), fill the holes, one level-1 candidate pass, then the vector median. */
static void lmc_a_from_b(te_context* ctx, VkCommandBuffer cb) {
    struct te_lmc* m = ctx->lmc;
    enum { RD_S = 2, WR_S = 4, FROM_B = 8 };
    uint32_t g = groups(m->n1, 64);
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cb, m->scratch.buf, (VkDeviceSize)m->off[1] * 16u, (VkDeviceSize)m->n1 * 16u, 0xffffffffu);
    lmc_transfer_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->search[1][1]);
    static const int pass[4] = {3, 4, 0, 2}, io[4] = {0, 0, WR_S | FROM_B, RD_S};
    for (int k = 0; k < 4; ++k) {
        lmc_push_c(ctx, cb, 0.0f, 1, 1, pass[k], 1 | io[k]);
        vkCmdDispatch(cb, g, 1, 1);
        compute_barrier(cb);
    }
}

static void lmc_record_analysis(te_context* ctx, te_slot* slot, const te_submit* s, int generate) {
    struct te_lmc* m = ctx->lmc;
    VkCommandBuffer cb = slot->analysis;
    uint32_t par = s->slot;
    uint32_t W = ctx->desc.width, H = ctx->desc.height;
    vkCmdResetQueryPool(cb, slot->queries, 0, TE_QUERIES);
    /* everything earlier in submission order (the previous pair's outputs read these buffers) */
    barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    if (ctx->upload_pending[par]) {
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {W, H, 1}};
        vkCmdCopyBufferToImage(cb, ctx->upload[par].buf, ctx->src[par].img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    }
    ownership(cb, &ctx->src[par], ctx->dev->qfi, 1);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot->queries, 0);
    slot->lmc_timed = 0;
    slot->lmc_split = 0;
    if (!generate) {
        /* history only: no pair; a skip or a hold also restarts every predictor */
        m->have_temporal = 0;
        pthread_mutex_lock(&m->fit_lock);
        m->prev_valid = 0;
        m->fit_gen++;
        pthread_mutex_unlock(&m->fit_lock);
        lmc_fill(cb, &m->layers, 0, 4, 0);                    /* layer_count */
        lmc_fill(cb, &m->globals, 4, 4, 0);                   /* moving */
        if(m->shared_flow){lmc_fill(cb,&m->globals,0,4,0);lmc_fill(cb,&m->globals,88,12,0);}
        if (ctx->frames > 0) {
            VkClearColorValue zero = {{0, 0, 0, 0}};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, m->hud.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        lmc_transfer_barrier(cb);
    } else {
        lmc_fill(cb, &m->layers, LMC_LY_LIVE + 4u * 2u * m->n1, 12, 0);  /* list length, total, stop */
        lmc_fill(cb, &m->globals, 8, 4, 0);                                /* moving votes */
        lmc_fill(cb, &m->globals, LMC_G_ORPHANS, 8, 0);                    /* orphan counts */
        if(m->shared_flow)lmc_fill(cb,&m->globals,92,4,0); /* keep previous cut for search */
        lmc_fill(cb, &m->globals, LMC_G_OHIST, LMC_G_OCLEAR, 0);           /* orphan selection */
        lmc_transfer_barrier(cb);
    }
    if(m->shared_flow){lmc_fill(cb,&m->globals,100,4,0);lmc_transfer_barrier(cb);}
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->layout, 0, 1, &m->set0[par], 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[LMC_PREP]);
    lmc_push_c(ctx, cb, 0.0f, 0, 0, 0, (generate?0:1)|(m->shared_flow?2:0));
    vkCmdDispatch(cb, groups(W, 8), groups(H, 8), 1);
    compute_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[LMC_DOWN]);
    for (int l = 0; l < LMC_LEVELS; ++l) {
        lmc_push_c(ctx, cb, 0.0f, l, 0, 0, 0);
        vkCmdDispatch(cb, groups(m->lw[l + 1], 8), groups(m->lh[l + 1], 8), 1);
        compute_barrier(cb);
    }
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 1);
    if (!generate) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 3);
        return;
    }
    slot->lmc_timed = 1;
#define LMC_TS(k) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, TE_LMC_Q0 + (k))
    LMC_TS(0);
    /* block search: B grid (temporal = previous pair), then A grid (temporal = this B field) */
    int tier = s->lmc_tier < 0 ? 0 : s->lmc_tier > TE_LMC_TIERS - 1 ? TE_LMC_TIERS - 1 : s->lmc_tier;
    if (m->n1 > 65536u && tier > 0) tier = 0;   /* the carry's key holds a 16-bit block index */
    slot->lmc_tier = tier;
    lmc_search_grid(ctx, cb, 0, m->have_temporal, tier >= 2 ? 1 : 2);
    LMC_TS(1 + TE_LMC_SEARCH_B);
    if (tier >= 1)
        lmc_a_from_b(ctx, cb);
    else
        lmc_search_grid(ctx, cb, 1, 1, 2);
    LMC_TS(1 + TE_LMC_SEARCH_A);
    if(m->compact_flow) {
        // One compact confidence/refinement phase for both output times.
        LMC_TS(1+TE_LMC_LAYERS);
        vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,slot->queries,2);
        LMC_TS(1+TE_LMC_FIT_WAIT);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_SF_PREPARE]);
        for(int g=0;g<2;++g) {
            if(g)compute_barrier(cb);
            lmc_push_c(ctx,cb,0,1,g,0,0);vkCmdDispatch(cb,groups(m->n1,64),1,1);
        }
        compute_barrier(cb);
        lmc_push_c(ctx,cb,0,1,0,2,0);vkCmdDispatch(cb,groups(m->n1,64),1,1);
        compute_barrier(cb);
        LMC_TS(1+TE_LMC_FIELD);LMC_TS(1+TE_LMC_MASKED);
        LMC_TS(1+TE_LMC_ORPHAN);LMC_TS(1+TE_LMC_FIELD2);
        m->have_temporal=1;
        vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,slot->queries,3);
        return;
    }
    /* block statistics for the CPU motion model (usable blocks, HUD blocks, motion
       votes); the planes and the palette themselves are fitted on the CPU */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[LMC_LAYERS]);
    lmc_push_c(ctx, cb, 0.0f, 1, 0, 0, (m->gpu_fit?1:0)|(m->shared_flow?16:0));
    vkCmdDispatch(cb, m->n1, 1, 1);   /* one workgroup per block */
    compute_barrier(cb);
    LMC_TS(1 + TE_LMC_LAYERS);
    if(m->shared_flow) {
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_SF_PREPARE]);
        lmc_push_c(ctx,cb,0,1,0,3,0);vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
        if(ctx->desc.shared_flow_dense>=2) {
            vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_SF_MODEL]);
            vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
        }
        if(ctx->desc.shared_flow_dense==4) {
            vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_SF_PREPARE]);
            lmc_push_c(ctx,cb,0,1,0,4,0);vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
            for(int layer=0;layer<LMC_MAX_LAYERS;++layer){
                vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_GPU_IRLS]);
                lmc_push_c(ctx,cb,0,1,0,1,32);vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
                vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_GPU_HYP]);
                lmc_push_c(ctx,cb,0,1,0,2,32);vkCmdDispatch(cb,LMC_HYPS+LMC_MAX_LAYERS,1,1);compute_barrier(cb);
                vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_GPU_IRLS]);
                lmc_push_c(ctx,cb,0,1,0,3,32);vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
            }
        }
        if(ctx->desc.shared_flow_dense>=3){
            vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_GPU_PALETTE]);
            lmc_push_c(ctx,cb,0,1,0,0,0);vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
        }
        vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,slot->queries,2);
    } else if(m->gpu_fit){
        for(int layer=0;layer<LMC_MAX_LAYERS;++layer){
            vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_GPU_IRLS]);
            lmc_push_c(ctx,cb,0,1,0,1,0);vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
            vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_GPU_HYP]);
            lmc_push_c(ctx,cb,0,1,0,2,0);vkCmdDispatch(cb,LMC_HYPS+LMC_MAX_LAYERS,1,1);compute_barrier(cb);
            vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_GPU_IRLS]);
            lmc_push_c(ctx,cb,0,1,0,3,0);vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
        }
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,m->pipe[LMC_GPU_PALETTE]);
        lmc_push_c(ctx,cb,0,1,0,0,0);vkCmdDispatch(cb,1,1,1);compute_barrier(cb);
        vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,slot->queries,2);
    } else {
    /* the input of the CPU motion model: vectors (+ HUD flag in w), usable flags, votes */
    int rs = (int)(slot - ctx->ring);
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy fv = {(VkDeviceSize)m->off[1] * 16, 0, (VkDeviceSize)m->n1 * 16};
    vkCmdCopyBuffer(cb, m->vec[0].buf, m->fitrb[rs].buf, 1, &fv);
    VkBufferCopy fl = {LMC_LY_LIVE, (VkDeviceSize)m->n1 * 16, (VkDeviceSize)m->n1 * 4};
    vkCmdCopyBuffer(cb, m->layers.buf, m->fitrb[rs].buf, 1, &fl);
    VkBufferCopy fvt = {8, (VkDeviceSize)m->n1 * 20, 4};
    vkCmdCopyBuffer(cb, m->globals.buf, m->fitrb[rs].buf, 1, &fvt);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 2);
    /* second submission: the fit worker submits it once the model of this pair is in stage */
    slot->lmc_seq = ++m->fit_seq;
    slot->lmc_split = 1;
    cb = slot->analysis2;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &bi);
    barrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy ml[2] = {{LMC_STAGE_COUNT, 0, 4}, {LMC_STAGE_LAYERS, LMC_LY_LAYERS, 48u * LMC_MAX_LAYERS}};
    vkCmdCopyBuffer(cb, m->stage[rs].buf, m->layers.buf, 2, ml);
    VkBufferCopy mg[2] = {{LMC_STAGE_HEAD, 0, 8}, {LMC_STAGE_PAL, 16, 16u * LMC_PALETTE}};
    vkCmdCopyBuffer(cb, m->stage[rs].buf, m->globals.buf, 2, mg);
    lmc_transfer_barrier(cb);
    }
    LMC_TS(1 + TE_LMC_FIT_WAIT);   /* end of the wait for the CPU fit (not GPU work) */
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->layout, 0, 1, &m->set0[par], 0, NULL);
    /* per-pixel fields, orphan objects, fields again next to orphans */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[LMC_FIELD]);
    for (int g = 0; g < 2; ++g) {
        if (g) compute_barrier(cb);  /* the shader names both fields (pass 1 samples them): see below */
        lmc_push_c(ctx, cb, 0.0f, 1, g, 0, 0);
        vkCmdDispatch(cb, groups(m->bw[1], 2), groups(m->bh[1], 2), 1);
    }
    compute_barrier(cb);
    LMC_TS(1 + TE_LMC_FIELD);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[LMC_MASKED]);
    lmc_push_c(ctx, cb, 0.0f, 0, 0, 0, 0);
    vkCmdDispatch(cb, groups(W / 4, 4), groups(H / 4, 4), 1);   /* 16x16-pixel tiles */
    compute_barrier(cb);
    LMC_TS(1 + TE_LMC_MASKED);
    /* orphans: count residual pixels per block, keep the strongest (at most ocap per grid), list them,
       then one workgroup per listed block */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[LMC_ORPHAN]);
    for (int g = 0; g < 2; ++g) {
        lmc_push_c(ctx, cb, 0.0f, 1, g, 0, 0);
        vkCmdDispatch(cb, m->n1, 1, 1);   /* one workgroup per block */
    }
    compute_barrier(cb);
    lmc_push_c(ctx, cb, 0.0f, 1, 0, 1, 0);
    vkCmdDispatch(cb, 1, 1, 1);
    compute_barrier(cb);
    for (int g = 0; g < 2; ++g) {
        lmc_push_c(ctx, cb, 0.0f, 1, g, 2, 0);
        vkCmdDispatch(cb, groups(m->n1, 64), 1, 1);
    }
    compute_barrier(cb);
    if (m->ocap) {
        for (int g = 0; g < 2; ++g) {
            lmc_push_c(ctx, cb, 0.0f, 1, g, 3, 0);
            vkCmdDispatch(cb, m->ocap, 1, 1);
        }
        compute_barrier(cb);
    }
    LMC_TS(1 + TE_LMC_ORPHAN);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[LMC_FIELD]);
    for (int g = 0; g < 2; ++g) {
        /* pass 1 reads the pass-0 field through the sampler: the shader names both grids' fields, so
           the second grid waits for the first one's writes too */
        if (g) compute_barrier(cb);
        lmc_push_c(ctx, cb, 0.0f, 1, g, 1, 0);
        vkCmdDispatch(cb, groups(m->bw[1], 2), groups(m->bh[1], 2), 1);
    }
    /* SF_PREPARE can write temporal[]. The following copy overwrites that
     * buffer; include transfer writes as well as the vector transfer read. */
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    LMC_TS(1 + TE_LMC_FIELD2);
#undef LMC_TS
    /* the B field of this pair is the next pair's temporal predictor */
    VkBufferCopy copy = {(VkDeviceSize)m->off[1] * 16, 0, (VkDeviceSize)m->n1 * 16};
    vkCmdCopyBuffer(cb, m->vec[0].buf, m->temporal.buf, 1, &copy);
    m->have_temporal = 1;
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, 3);
    if(!m->gpu_fit) vkEndCommandBuffer(cb);
}

static void lmc_record_output(te_context* ctx, te_slot* slot, const te_submit* s, uint32_t i, int hold) {
    struct te_lmc* m = ctx->lmc;
    VkCommandBuffer cb = slot->out[i];
    uint32_t par = s->slot;
    uint32_t q = 4 + i * 6;
    ownership(cb, &ctx->dst[i], ctx->dev->qfi, 1);
    compute_barrier(cb);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, q + 0);
    VkDescriptorSet sets[2] = {m->set0[par], m->set1[i]};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->layout, 0, 2, sets, 0, NULL);
    float t = hold ? 1.0f : s->timestamps[i];
    if (!hold && !m->compact_flow) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[LMC_SELECT]);
        lmc_push_c(ctx, cb, t, 0, 0, 0, 0);
        vkCmdDispatch(cb, groups(m->w2, 8), groups(m->h2, 8), 1);
        compute_barrier(cb);
    }
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, q + 1);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m->pipe[m->compact_flow?LMC_SF_COMPOSE:LMC_COMPOSE]);
    lmc_push_c(ctx, cb, t, 0, 0, 0, (hold ? 1 : 0)|(m->shared_flow&&!m->compact_flow?16:0));
    vkCmdDispatch(cb, groups(ctx->desc.width, 8), groups(ctx->desc.height, 8), 1);
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    for (uint32_t k = 2; k <= 5; ++k)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, slot->queries, q + k);
    if (s->download && ctx->desc.host_io) {
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {ctx->desc.width, ctx->desc.height, 1}};
        vkCmdCopyImageToBuffer(cb, ctx->dst[i].img, VK_IMAGE_LAYOUT_GENERAL, ctx->download[i].buf, 1, &region);
    }
    /* telemetry: layer count + shares and orphan counts into the readback header */
    VkBufferCopy c1 = {0, (VkDeviceSize)TE_GLOBALS_HEADER * i, 4};
    vkCmdCopyBuffer(cb, m->layers.buf, slot->readback.buf, 1, &c1);
    if(m->shared_flow) {
        VkBufferCopy state[4]={{m->compact_flow?0:88,(VkDeviceSize)TE_GLOBALS_HEADER*i+12,4},
                              {96,(VkDeviceSize)TE_GLOBALS_HEADER*i+16,4},
                              {92,(VkDeviceSize)TE_GLOBALS_HEADER*i+20,4},
                              {100,(VkDeviceSize)TE_GLOBALS_HEADER*i+24,4}};
        vkCmdCopyBuffer(cb,m->globals.buf,slot->readback.buf,4,state);
    }
    VkBufferCopy c2 = {LMC_G_ORPHANS, (VkDeviceSize)TE_GLOBALS_HEADER * i + 4, 8};
    vkCmdCopyBuffer(cb, m->globals.buf, slot->readback.buf, 1, &c2);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
    ownership(cb, &ctx->dst[i], ctx->dev->qfi, 0);
}

// GFG Temporal Engine, LMC path (layered motion compensation): shared
// declarations.  Every shader mirrors a function of bench/gen_lmc2.py (and the
// helpers it takes from bench/gen_lmc.py); keep both in step.
//
// Set 0 is per source parity: B is the newest real frame (slot s), A the
// previous one (slot s ^ 1).  Set 1 holds one destination image.
//
// Vector convention: v is the motion from A to B; a point at x in A is at x + v
// in B.  Units are the pixels of the grid the vector lives on.
#extension GL_EXT_control_flow_attributes : enable

#define LMC_LEVELS 4      // pyramid levels below full resolution: 1/2 .. 1/16
#define LMC_BLK 4         // block side in level pixels
#define LMC_MAX_LAYERS 3
#define LMC_PALETTE 4
#define LMC_HYPS 128      // RANSAC hypotheses (one workgroup each)
#define LMC_ORPHAN_MAX 1024  // orphan list capacity per grid (P.orphan_cap <= this)

layout(std140, set = 0, binding = 0) uniform LmcParams {
    ivec4 size;           // x, y full res; z, w half res
    ivec4 lsize[5];       // level l: xy = pixels, zw = blocks (LMC_BLK)
    ivec4 blk_off;        // block offset of level l (1..4) in the per-level buffers: x=l1 y=l2 z=l3 w=l4
    ivec4 counts;         // x = blocks of all levels, y = level-1 blocks, z = half pixels, w = full pixels
    float pen_update;     // 3DRS penalty per level pixel of update
    float smooth_;        // propagation smoothness per level pixel
    float margin;         // half-res field: other candidates must beat the own block by this
    float tol_px;         // trajectory consistency (half-res px, + 10 % of |v|)
    float still;          // HUD: max |B - A| below this is "still"
    float orphan_resid;   // half-res 3x3 cost above which a pixel is unexplained
    float orphan_share;   // unexplained share that makes a block an orphan
    float orphan_max_cost;
    int coarse_r;         // full search radius at 1/16
    int propagate;        // propagation sweeps per level
    int hud_pairs;
    int orphan_r;         // full-res radius of the orphan search
    float min_layer_share;
    int ransac_iters;     // == LMC_HYPS
    int orphan_cap;       // orphan blocks searched per grid, strongest first
    int pad1;
} P;

layout(set = 0, binding = 1) uniform sampler2D srcA;
layout(set = 0, binding = 2) uniform sampler2D srcB;
layout(set = 0, binding = 3) uniform sampler2D lumaA0;
layout(set = 0, binding = 4) uniform sampler2D lumaA1;
layout(set = 0, binding = 5) uniform sampler2D lumaA2;
layout(set = 0, binding = 6) uniform sampler2D lumaA3;
layout(set = 0, binding = 7) uniform sampler2D lumaA4;
layout(set = 0, binding = 8) uniform sampler2D lumaB0;
layout(set = 0, binding = 9) uniform sampler2D lumaB1;
layout(set = 0, binding = 10) uniform sampler2D lumaB2;
layout(set = 0, binding = 11) uniform sampler2D lumaB3;
layout(set = 0, binding = 12) uniform sampler2D lumaB4;
layout(set = 0, binding = 18) uniform sampler2D halfA;
layout(set = 0, binding = 19) uniform sampler2D halfB;

// Block vectors of every level for one grid: xy = vector (level px), z = cost.
layout(std430, set = 0, binding = 22) buffer VecB { vec4 vb[]; };
layout(std430, set = 0, binding = 23) buffer VecA { vec4 va[]; };
layout(std430, set = 0, binding = 24) buffer Scratch { vec4 scratch[]; };
// Previous pair's level-1 B-grid field (level-1 px, a copy of vb), the temporal predictor.
layout(std430, set = 0, binding = 25) buffer Temporal { vec4 temporal[]; };

struct Layer { vec4 h0; vec4 h1; vec4 h2; };   // rows of H (A -> B, full-res px); h2.w = share
layout(std430, set = 0, binding = 26) buffer Layers {
    uint layer_count;
    uint prev_count;
    uint pad_l0, pad_l1;
    Layer layers[LMC_MAX_LAYERS];
    Layer prev_layers[LMC_MAX_LAYERS];
    Layer hyps[LMC_HYPS + LMC_MAX_LAYERS];     // prev layers are hypotheses too
    uint hyp_inliers[LMC_HYPS + LMC_MAX_LAYERS];
    uint live[];                               // per level-1 block: 1 while unexplained and usable
} LY;

layout(std430, set = 0, binding = 27) buffer Globals {
    uint pal_count;
    uint moving;          // the previous pair moved: HUD counter may advance
    uint moving_votes;    // blocks with max |v| > 1 (this pair)
    uint pairs;           // pair counter (RANSAC seed)
    vec4 palette[LMC_PALETTE];
    uint orphans_b, orphans_a, pad_g0, pad_g1;
    uint hist[64 * 64];   // unused (the palette histogram before the CPU fit); keeps the layout
    uint ohist[2 * 65];   // per grid: orphan candidates by residual pixel count (0..64)
    uint othr[2];         // per grid: blocks with a count above this are searched
    uint olist_n[2];
    uint olist[2 * LMC_ORPHAN_MAX];
} G;

// Half-res per-pixel fields (rgba16f): xy = vector (half px), z = cost.
// Sampled for the trajectory test (hardware bilinear), stored by lmc_field.
layout(set = 0, binding = 28) uniform sampler2D fieldB;
layout(set = 0, binding = 29) uniform sampler2D fieldA;
// Orphan block vectors (level-1 grid): xy = vector (full-res px), z = 1 when valid.
layout(std430, set = 0, binding = 30) buffer OrphansB { vec4 ob[]; } ORB;
layout(std430, set = 0, binding = 31) buffer OrphansA { vec4 oa[]; } ORA;
// 1/4-res masked luma and mask max of both frames for the orphan search.
layout(std430, set = 0, binding = 32) buffer Masked { vec4 mq[]; } MQ;  // x yB, y mB, z yA, w mA
// Trajectory choice at half res for the current output: xy = vector, z = mode.
layout(std430, set = 0, binding = 33) buffer Select { vec4 sel[]; } SEL;

layout(push_constant) uniform Push {
    float t;
    int level;
    int grid;       // 0 = B grid, 1 = A grid
    int pass_;      // pass-specific selector
    int parity;
    int flags;
    float batch_times[4];
} PC;

vec4 orphan_at(int grid, int i) { return grid == 0 ? ORB.ob[i] : ORA.oa[i]; }

// ------------------------------------------------------------------ sampling

ivec2 lpix(int l) { return P.lsize[l].xy; }
ivec2 lblocks(int l) { return P.lsize[l].zw; }
int blk_offset(int l) { return l == 1 ? P.blk_off.x : l == 2 ? P.blk_off.y : l == 3 ? P.blk_off.z : P.blk_off.w; }

// reference bilinear(): pixel-index coordinates, clamp to [0, n - 1.0001]
float luma_a(int l, vec2 q) {
    vec2 s = vec2(lpix(l));
    vec2 uv = (clamp(q, vec2(0.0), s - 1.0) + 0.5) / s;
    if (l == 0) return textureLod(lumaA0, uv, 0.0).r;
    if (l == 1) return textureLod(lumaA1, uv, 0.0).r;
    if (l == 2) return textureLod(lumaA2, uv, 0.0).r;
    if (l == 3) return textureLod(lumaA3, uv, 0.0).r;
    return textureLod(lumaA4, uv, 0.0).r;
}

float luma_b(int l, vec2 q) {
    vec2 s = vec2(lpix(l));
    vec2 uv = (clamp(q, vec2(0.0), s - 1.0) + 0.5) / s;
    if (l == 0) return textureLod(lumaB0, uv, 0.0).r;
    if (l == 1) return textureLod(lumaB1, uv, 0.0).r;
    if (l == 2) return textureLod(lumaB2, uv, 0.0).r;
    if (l == 3) return textureLod(lumaB3, uv, 0.0).r;
    return textureLod(lumaB4, uv, 0.0).r;
}

float luma_at(int which, int l, vec2 q) { return which == 0 ? luma_a(l, q) : luma_b(l, q); }

// 2x2 texels of level l with top-left texel t (clamped to the edge like the
// reference's clamp): x = (t.x, t.y + 1), y = (t.x + 1, t.y + 1), z = (t.x + 1, t.y), w = t
vec4 gather_a(int l, ivec2 t) {
    vec2 uv = vec2(t + 1) / vec2(lpix(l));
    if (l == 0) return textureGather(lumaA0, uv, 0);
    if (l == 1) return textureGather(lumaA1, uv, 0);
    if (l == 2) return textureGather(lumaA2, uv, 0);
    if (l == 3) return textureGather(lumaA3, uv, 0);
    return textureGather(lumaA4, uv, 0);
}

vec4 gather_b(int l, ivec2 t) {
    vec2 uv = vec2(t + 1) / vec2(lpix(l));
    if (l == 0) return textureGather(lumaB0, uv, 0);
    if (l == 1) return textureGather(lumaB1, uv, 0);
    if (l == 2) return textureGather(lumaB2, uv, 0);
    if (l == 3) return textureGather(lumaB3, uv, 0);
    return textureGather(lumaB4, uv, 0);
}

vec3 half_rgb(int which, vec2 q) {
    vec2 s = vec2(P.size.zw);
    vec2 uv = (clamp(q, vec2(0.0), s - 1.0) + 0.5) / s;
    return which == 0 ? textureLod(halfA, uv, 0.0).rgb : textureLod(halfB, uv, 0.0).rgb;
}

vec3 full_rgb(int which, vec2 q) {
    vec2 s = vec2(P.size.xy);
    vec2 uv = (clamp(q, vec2(0.0), s - 1.0) + 0.5) / s;
    return which == 0 ? textureLod(srcA, uv, 0.0).rgb : textureLod(srcB, uv, 0.0).rgb;
}

bool inside(vec2 q, ivec2 n) { return all(greaterThanEqual(q, vec2(0.0))) && all(lessThanEqual(q, vec2(n) - 1.0)); }

// ------------------------------------------------------------------ homographies

vec2 apply_h(Layer L, vec2 x) {
    float d = L.h2.x * x.x + L.h2.y * x.y + L.h2.z;
    return vec2(L.h0.x * x.x + L.h0.y * x.y + L.h0.z, L.h1.x * x.x + L.h1.y * x.y + L.h1.z) / d;
}

// inverse of a 3x3 (rows h0, h1, h2), normalised so h[2][2] = 1
Layer inv_h(Layer L) {
    mat3 m = transpose(mat3(L.h0.xyz, L.h1.xyz, L.h2.xyz));  // GLSL columns = rows of H
    mat3 i = inverse(m);
    i /= i[2][2];
    mat3 r = transpose(i);
    Layer o;
    o.h0 = vec4(r[0], 0.0);
    o.h1 = vec4(r[1], 0.0);
    o.h2 = vec4(r[2], L.h2.w);
    return o;
}

// Camera motion (A -> B) at grid point g of a grid with this scale (reference camera_vec):
// grid 1 (A): H(x) - x; grid 0 (B): x - H^-1(x); in the grid's own pixels.
vec2 camera_vec(Layer L, Layer Li, vec2 g, float scale, int grid) {
    vec2 f = (g + 0.5) * scale - 0.5;
    if (grid == 1) return (apply_h(L, f) - f) / scale;
    return (f - apply_h(Li, f)) / scale;
}

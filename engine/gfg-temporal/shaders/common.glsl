// GFG Temporal Engine: shared declarations.  Every shader mirrors a function of
// reference/gfg_temporal_ref/core.py; keep both in step.
//
// Descriptor set 0 is per source parity: B (newest real frame) is slot s,
// A (previous real frame) is slot s ^ 1.  Set 1 holds one destination image.

#extension GL_EXT_control_flow_attributes : enable

#define CLASS_REUSE 0u
#define CLASS_LIGHT 1u
#define CLASS_FULL 2u
#define CLASS_NONE 255u

layout(std140, set = 0, binding = 0) uniform Params {
    ivec2 size;            // full-resolution width, height
    ivec2 tiles;           // tile grid
    float static_thr;
    float static_max_thr;
    float light_thr;
    float cut_thr;
    float residual_thr;
    float occ_lo;
    float occ_hi;
    float smooth_bias;
    float me_bias;
    int coarse_range;
    int hysteresis;
    int levels;
    int adaptive;
    int force_class;
    int tile;
    int probe;
    int full_density;      // FULL decision per 1x1 or 2x2 block
    int pad_a, pad_b, pad_c;
    vec4 level_size[4];    // xy = level width, height
} P;

// Encode channel order on the GPU. A raw compatible-format image copy into
// a BGRA/sRGB WSI image preserves encoded colour bytes without a CPU swizzle
// or an additional sRGB transfer-function conversion.
vec4 output_color(vec4 value) { return P.pad_a != 0 ? value.bgra : value; }

layout(set = 0, binding = 1) uniform sampler2D srcA;
layout(set = 0, binding = 2) uniform sampler2D srcB;
layout(set = 0, binding = 3) uniform sampler2D lumaA0;
layout(set = 0, binding = 4) uniform sampler2D lumaA1;
layout(set = 0, binding = 5) uniform sampler2D lumaA2;
layout(set = 0, binding = 6) uniform sampler2D lumaA3;
layout(set = 0, binding = 7) uniform sampler2D lumaB0;
layout(set = 0, binding = 8) uniform sampler2D lumaB1;
layout(set = 0, binding = 9) uniform sampler2D lumaB2;
layout(set = 0, binding = 10) uniform sampler2D lumaB3;

struct TileStats {
    float err;         // E
    float diff;        // E0
    float diff_max;    // M0
    float probe;       // P
    float smooth_;     // S
    float neigh;       // max |v|_inf over 3x3
    float pad0;
    float pad1;
};

struct TileState {
    uint state;        // hysteresis class
    uint count;        // consecutive eligible downgrade pairs
    uint frame_cls;    // class used by the last executed output frame
    uint target;       // target class of this pair (diagnostic)
};

layout(std430, set = 0, binding = 11) buffer Vectors { vec2 vec_cur[]; };   // levels x tiles
layout(std430, set = 0, binding = 12) readonly buffer PrevVectors { vec2 vec_prev[]; };
layout(std430, set = 0, binding = 13) buffer Stats { TileStats stats[]; };
layout(std430, set = 0, binding = 14) buffer States { TileState tstate[]; };
layout(std430, set = 0, binding = 15) buffer Residual { float residual[]; };

// Globals: per-parity counters and the three tile lists with indirect args.
layout(std430, set = 0, binding = 16) buffer Globals {
    uint count_err[2];
    uint count_diff[2];
    uint history_valid[2];
    uint cut[2];
    uint conf_milli[2];
    uint promoted;
    uint pad_g;
    uvec4 args[3];         // x = tile count, y = z = 1 (indirect dispatch), w unused
    uint list[];           // 3 * tiles entries: class c list starts at c * tiles
} G;

layout(push_constant) uniform Push {
    float t;
    int level;
    int frame;
    int parity;
    float step_;
    int flags;
} PC;

int tile_count() { return P.tiles.x * P.tiles.y; }

vec2 level_size(int l) { return P.level_size[l].xy; }

float luma_a(int l, vec2 q) {
    vec2 uv = (q + 0.5) / level_size(l);
    if (l == 0) return textureLod(lumaA0, uv, 0.0).r;
    if (l == 1) return textureLod(lumaA1, uv, 0.0).r;
    if (l == 2) return textureLod(lumaA2, uv, 0.0).r;
    return textureLod(lumaA3, uv, 0.0).r;
}

float luma_b(int l, vec2 q) {
    vec2 uv = (q + 0.5) / level_size(l);
    if (l == 0) return textureLod(lumaB0, uv, 0.0).r;
    if (l == 1) return textureLod(lumaB1, uv, 0.0).r;
    if (l == 2) return textureLod(lumaB2, uv, 0.0).r;
    return textureLod(lumaB3, uv, 0.0).r;
}

vec3 rgb_a(vec2 q) { return textureLod(srcA, (q + 0.5) / vec2(P.size), 0.0).rgb; }
vec3 rgb_b(vec2 q) { return textureLod(srcB, (q + 0.5) / vec2(P.size), 0.0).rgb; }

vec2 tile_vector(int level, ivec2 tc) {
    tc = clamp(tc, ivec2(0), P.tiles - 1);
    return vec_cur[level * tile_count() + tc.y * P.tiles.x + tc.x];
}

// Bilinear interpolation of the level-0 tile vectors (reference: vector_field).
vec2 vector_field(vec2 p) {
    float T = float(P.tile);
    vec2 u = clamp((p - (T - 1.0) * 0.5) / T, vec2(0.0), vec2(P.tiles - 1));
    vec2 f0 = floor(u);
    vec2 fr = u - f0;
    ivec2 i0 = ivec2(f0);
    ivec2 i1 = min(i0 + 1, P.tiles - 1);
    vec2 v00 = tile_vector(0, i0);
    vec2 v01 = tile_vector(0, ivec2(i1.x, i0.y));
    vec2 v10 = tile_vector(0, ivec2(i0.x, i1.y));
    vec2 v11 = tile_vector(0, i1);
    vec2 top = v00 + (v01 - v00) * fr.x;
    vec2 bot = v10 + (v11 - v10) * fr.x;
    return top + (bot - top) * fr.y;
}

// Field at the trajectory through p at time t (reference: smooth_vector).
vec2 smooth_vector(vec2 p, float t) {
    vec2 v = vector_field(p);
    return vector_field(p + (0.5 - t) * v);
}

// ME window (reference: me_window).
int me_n(int level) { int span = max(1, P.tile >> level); return min(8, max(4, span)); }
float me_step(int level) { int span = max(1, P.tile >> level); return float(max(1, span / me_n(level))); }

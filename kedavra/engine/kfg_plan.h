// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame generation: GPU pass plan.
//
// The plan is the single description of which compute passes run, in what
// order, on which images and with which constants. Hosts (the standalone
// Vulkan harness and the Gamescope integration) only map image roles to their
// own textures, bind them in the listed slots and dispatch. Nothing here
// touches Vulkan, so the plan is shared verbatim between hosts.
//
// Per source frame (when it is latched):  analysis of the pair (A = previous, B = new)
//   reduce   colour B   -> luma B level 1 (1/2 resolution)
//   reduce   luma B L1  -> L2 -> L3 -> L4 (1/16)
//   me       L4 A,B               -> flow L4   (bidirectional)
//   me       L3 A,B + flow L4     -> flow L3
//   me       L2 A,B + flow L3     -> flow L2   (one vector pair per 4x4 pixels)
//   stats    L2 A,B + flow L2     -> stats     (scene cut)
// Per generated output at phase t:
//   select   L1 A,B + flow L2 + stats -> selection (4x4 blocks)
//   warp     colour A,B + selection + stats -> output (full resolution)
#ifndef KFG_PLAN_H
#define KFG_PLAN_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KFG_LEVELS 4      // luma levels 1..4
#define KFG_RING 4        // frames / pairs kept so delayed outputs can still use their pair
#define KFG_MAX_SLOTS 4
#define KFG_MAX_PASSES 16

enum kfg_shader {
	KFG_SHADER_REDUCE = 0,
	KFG_SHADER_ME,
	KFG_SHADER_STATS,
	KFG_SHADER_SELECT,
	KFG_SHADER_WARP,
	KFG_SHADER_COUNT
};

static const char *const kfg_shader_names[KFG_SHADER_COUNT] = {
	"kfg_reduce", "kfg_me", "kfg_stats", "kfg_select", "kfg_warp",
};

// Image roles. Ring-indexed roles carry the ring index in kfg_image_ref.ring.
enum kfg_role {
	KFG_ROLE_NONE = 0,
	KFG_ROLE_COLOR,     // source colour of a frame (host-owned, e.g. a compositor commit)
	KFG_ROLE_LUMA,      // luma pyramid of a frame; level 1..4
	KFG_ROLE_FLOW,      // bidirectional flow of a pair; level 2..4
	KFG_ROLE_STATS,     // 1x1 pair statistics
	KFG_ROLE_SELECT,    // per-output selection field (transient)
	KFG_ROLE_OUTPUT,    // generated output (host-owned)
};

// Formats requested for internal roles (hosts translate to their own enums).
enum kfg_format {
	KFG_FORMAT_R8 = 0,         // luma
	KFG_FORMAT_RGBA16F,        // flow, selection, stats
	KFG_FORMAT_RGBA8,          // output
};

typedef struct kfg_image_ref {
	enum kfg_role role;
	uint32_t ring;   // frame ring index (COLOR, LUMA) or pair ring index (FLOW, STATS)
	uint32_t level;  // LUMA / FLOW level
} kfg_image_ref;

typedef struct kfg_params {
	// Layout matches kfg_common.glsl (scalar block layout).
	int32_t dst_size[2];
	int32_t src_size[2];
	int32_t full_size[2];
	float t;
	int32_t level;
	int32_t radius;
	int32_t flags;
	float lambda;
	float cut_threshold;
	float pad0;
	float pad1;
} kfg_params;

#define KFG_FLAG_COARSEST 1
#define KFG_FLAG_DEBUG_MARKER 2
#define KFG_FLAG_LUMA_INPUT 4
#define KFG_FLAG_TENT 8        // reduce: 4x4 tent instead of 2x2 box (coarse search levels)

typedef struct kfg_pass {
	enum kfg_shader shader;
	kfg_image_ref dst;
	kfg_image_ref slots[KFG_MAX_SLOTS];
	uint32_t nslots;
	kfg_params params;
	uint32_t groups[2];   // workgroup counts
	const char *label;
} kfg_pass;

typedef struct kfg_plan {
	kfg_pass pass[KFG_MAX_PASSES];
	uint32_t n;
} kfg_plan;

typedef struct kfg_tuning {
	int32_t radius_coarsest;   // texels at level 4 (x16 pixels)
	int32_t radius_refine;     // texels at levels 3 and 2
	float lambda;
	float cut_threshold;       // fraction of badly explained blocks
} kfg_tuning;

static inline kfg_tuning kfg_default_tuning(void)
{
	kfg_tuning t;
	t.radius_coarsest = 6;  // +-96 pixels per source frame before refinement
	t.radius_refine = 1;
	t.lambda = 0.02f;
	t.cut_threshold = 0.5f;
	return t;
}

static inline uint32_t kfg_div_up(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

// Size of luma/flow level l (l >= 1) for a full-resolution w x h source.
static inline void kfg_level_size(uint32_t w, uint32_t h, uint32_t l, uint32_t out[2])
{
	uint32_t x = w, y = h;
	for (uint32_t i = 0; i < l; i++) {
		x = (x + 1u) / 2u;
		y = (y + 1u) / 2u;
	}
	out[0] = x ? x : 1u;
	out[1] = y ? y : 1u;
}

// Size of the selection field (one texel per 4x4 pixels) - equals level 2.
static inline void kfg_select_size(uint32_t w, uint32_t h, uint32_t out[2])
{
	out[0] = kfg_div_up(w, 4u);
	out[1] = kfg_div_up(h, 4u);
}

static inline kfg_image_ref kfg_ref(enum kfg_role role, uint32_t ring, uint32_t level)
{
	kfg_image_ref r;
	r.role = role;
	r.ring = ring;
	r.level = level;
	return r;
}

static inline kfg_pass *kfg_plan_add(kfg_plan *p, enum kfg_shader sh, kfg_image_ref dst, const char *label)
{
	kfg_pass *x = &p->pass[p->n++];
	memset(x, 0, sizeof(*x));
	x->shader = sh;
	x->dst = dst;
	x->label = label;
	return x;
}

static inline void kfg_set_domain(kfg_pass *x, const uint32_t dst[2], uint32_t gx, uint32_t gy)
{
	x->params.dst_size[0] = (int32_t)dst[0];
	x->params.dst_size[1] = (int32_t)dst[1];
	x->groups[0] = kfg_div_up(dst[0], gx);
	x->groups[1] = kfg_div_up(dst[1], gy);
}

// Analysis of the pair (frame ring fa -> frame ring fb), written to pair ring pr.
// have_prev = 0 for the first frame after a reset: only the pyramid of B is built.
static inline void kfg_plan_analyze(kfg_plan *p, uint32_t w, uint32_t h, uint32_t fa, uint32_t fb, uint32_t pr,
				    int have_prev, const kfg_tuning *tune)
{
	p->n = 0;
	uint32_t sz[KFG_LEVELS + 1][2];
	sz[0][0] = w;
	sz[0][1] = h;
	for (uint32_t l = 1; l <= KFG_LEVELS; l++)
		kfg_level_size(w, h, l, sz[l]);

	for (uint32_t l = 1; l <= KFG_LEVELS; l++) {
		kfg_pass *x = kfg_plan_add(p, KFG_SHADER_REDUCE, kfg_ref(KFG_ROLE_LUMA, fb, l), "reduce");
		x->slots[0] = l == 1 ? kfg_ref(KFG_ROLE_COLOR, fb, 0) : kfg_ref(KFG_ROLE_LUMA, fb, l - 1);
		x->nslots = 1;
		x->params.src_size[0] = (int32_t)sz[l - 1][0];
		x->params.src_size[1] = (int32_t)sz[l - 1][1];
		x->params.flags = l == 1 ? 0 : KFG_FLAG_LUMA_INPUT | (l >= 3 ? KFG_FLAG_TENT : 0);
		kfg_set_domain(x, sz[l], 8, 8);
	}
	if (!have_prev)
		return;
	for (uint32_t l = KFG_LEVELS; l >= 2; l--) {
		kfg_pass *x = kfg_plan_add(p, KFG_SHADER_ME, kfg_ref(KFG_ROLE_FLOW, pr, l), "me");
		x->slots[0] = kfg_ref(KFG_ROLE_LUMA, fa, l);
		x->slots[1] = kfg_ref(KFG_ROLE_LUMA, fb, l);
		x->slots[2] = l == KFG_LEVELS ? kfg_ref(KFG_ROLE_NONE, 0, 0) : kfg_ref(KFG_ROLE_FLOW, pr, l + 1);
		x->nslots = 3;
		x->params.src_size[0] = (int32_t)sz[l][0];
		x->params.src_size[1] = (int32_t)sz[l][1];
		x->params.full_size[0] = (int32_t)w;
		x->params.full_size[1] = (int32_t)h;
		x->params.level = (int32_t)l;
		x->params.radius = l == KFG_LEVELS ? tune->radius_coarsest : tune->radius_refine;
		x->params.flags = l == KFG_LEVELS ? KFG_FLAG_COARSEST : 0;
		x->params.lambda = tune->lambda;
		kfg_set_domain(x, sz[l], 8, 8);
	}
	kfg_pass *x = kfg_plan_add(p, KFG_SHADER_STATS, kfg_ref(KFG_ROLE_STATS, pr, 0), "stats");
	x->slots[0] = kfg_ref(KFG_ROLE_LUMA, fa, 2);
	x->slots[1] = kfg_ref(KFG_ROLE_LUMA, fb, 2);
	x->slots[2] = kfg_ref(KFG_ROLE_FLOW, pr, 2);
	x->nslots = 3;
	x->params.src_size[0] = (int32_t)sz[2][0];
	x->params.src_size[1] = (int32_t)sz[2][1];
	x->params.cut_threshold = tune->cut_threshold;
	x->params.dst_size[0] = 1;
	x->params.dst_size[1] = 1;
	x->groups[0] = 1;
	x->groups[1] = 1;
}

// One generated output at phase t in (0, 1) between frames fa and fb of pair pr.
static inline void kfg_plan_generate(kfg_plan *p, uint32_t w, uint32_t h, uint32_t fa, uint32_t fb, uint32_t pr,
				     float t, int32_t flags)
{
	p->n = 0;
	uint32_t l1[2], bs[2], full[2] = { w, h };
	kfg_level_size(w, h, 1, l1);
	kfg_select_size(w, h, bs);

	kfg_pass *x = kfg_plan_add(p, KFG_SHADER_SELECT, kfg_ref(KFG_ROLE_SELECT, 0, 0), "select");
	x->slots[0] = kfg_ref(KFG_ROLE_LUMA, fa, 1);
	x->slots[1] = kfg_ref(KFG_ROLE_LUMA, fb, 1);
	x->slots[2] = kfg_ref(KFG_ROLE_FLOW, pr, 2);
	x->slots[3] = kfg_ref(KFG_ROLE_STATS, pr, 0);
	x->nslots = 4;
	x->params.src_size[0] = (int32_t)l1[0];
	x->params.src_size[1] = (int32_t)l1[1];
	x->params.full_size[0] = (int32_t)w;
	x->params.full_size[1] = (int32_t)h;
	x->params.t = t;
	kfg_set_domain(x, bs, 8, 8);

	x = kfg_plan_add(p, KFG_SHADER_WARP, kfg_ref(KFG_ROLE_OUTPUT, 0, 0), "warp");
	x->slots[0] = kfg_ref(KFG_ROLE_COLOR, fa, 0);
	x->slots[1] = kfg_ref(KFG_ROLE_COLOR, fb, 0);
	x->slots[2] = kfg_ref(KFG_ROLE_SELECT, 0, 0);
	x->slots[3] = kfg_ref(KFG_ROLE_STATS, pr, 0);
	x->nslots = 4;
	x->params.src_size[0] = (int32_t)w;
	x->params.src_size[1] = (int32_t)h;
	x->params.full_size[0] = (int32_t)w;
	x->params.full_size[1] = (int32_t)h;
	x->params.t = t;
	x->params.flags = flags;
	kfg_set_domain(x, full, 8, 8);
}

// Format of an internal role.
static inline enum kfg_format kfg_role_format(enum kfg_role r)
{
	switch (r) {
	case KFG_ROLE_LUMA: return KFG_FORMAT_R8;
	case KFG_ROLE_OUTPUT: return KFG_FORMAT_RGBA8;
	default: return KFG_FORMAT_RGBA16F;
	}
}

#ifdef __cplusplus
}
#endif
#endif

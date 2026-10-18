/* Frame budget scheduler for the Temporal Engine (pure logic, no Vulkan).
 *
 * Inputs are measured, never assumed: GPU stage times from timestamp queries,
 * class counts from the engine, the real-frame interval and the GPU busy share
 * reported by the caller.  The scheduler only chooses engine knobs and
 * recommends how many synthetic frames are worth requesting; presentation and
 * pacing stay with the caller (Frame OS in GFG).
 */
#ifndef TE_SCHEDULER_H
#define TE_SCHEDULER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TE_SCHED_LEVELS 5          /* last level: FULL tiles run as LIGHT */
#define TE_SCHED_DEFAULT_CAP_MS 2.0 /* GPU ms per generated frame, see docs */

typedef enum {
    TE_SCHED_GENERATE = 0,   /* run the engine with the returned knobs */
    TE_SCHED_FALLBACK = 1,   /* do not generate: budget cannot be met or the engine is unhealthy */
} te_sched_action;

typedef enum {
    TE_REASON_NONE = 0,
    TE_REASON_WARMUP,            /* not enough measurements yet: conservative defaults */
    TE_REASON_OVER_BUDGET,       /* predicted cost above the budget at the cheapest level */
    TE_REASON_MISSES,            /* repeated deadline misses */
    TE_REASON_ADAPTIVE_NOT_WORTH,/* classification overhead above what it saves: uniform FULL */
    TE_REASON_SCENE_CUT,         /* last pair was a hold */
} te_sched_reason;

typedef struct {
    double real_interval_ms;   /* measured interval between real frames */
    double gpu_busy;           /* share of the interval the game keeps the GPU busy, <0 unknown */
    uint32_t requested;        /* synthetic frames per pair the caller wants */
} te_sched_input;

typedef struct {
    te_sched_action action;
    te_sched_reason reason;
    uint32_t outputs;          /* recommended synthetic frames per pair (<= requested) */
    uint32_t level;            /* 0 = best quality .. TE_SCHED_LEVELS-1 = cheapest */
    float light_scale;         /* LIGHT threshold multiplier for this level */
    int force_full;            /* classification disabled */
    int force_light;           /* cheapest level: FULL tiles run as LIGHT */
    int profile;               /* time executors separately on this pair (calibration) */
    double budget_ms;          /* GPU time available to frame generation per pair */
    double predicted_ms;       /* predicted GPU time of the pair with these knobs */
} te_sched_decision;

typedef struct {
    double cap_ms;             /* hard GPU cost cap per generated frame (0: none) */
    double fixed_ms;           /* pyramid + analyze + classify, EMA */
    double lists_ms;           /* per output, EMA */
    double tile_ms[3];         /* per tile per output: reuse, light, full (from profiled pairs) */
    double share[3];           /* class shares of the last pair */
    double frame_ms;           /* measured per-output total, EMA */
    uint32_t samples;          /* observed pairs */
    uint32_t calibrated;       /* profiled pairs */
    uint32_t level;
    uint32_t calm;             /* consecutive pairs well under budget */
    uint32_t misses;           /* consecutive pairs over budget */
    uint32_t not_worth;        /* consecutive pairs where adaptivity did not pay */
    int force_full;
    uint32_t full_since;
    uint32_t since_profile;
    uint32_t last_cut;
    uint32_t total_misses, total_pairs, fallbacks;
} te_scheduler;

void te_scheduler_init(te_scheduler* s);
float te_scheduler_light_scale(uint32_t level);
te_sched_decision te_scheduler_decide(te_scheduler* s, const te_sched_input* in);

/* Feed the measured statistics of a completed pair (from te_pair_stats). */
typedef struct {
    int timed, profiled;
    uint32_t outputs, tile_total, scene_cut;
    double fixed_ms;           /* pyramid + analyze + classify */
    double classify_ms;
    double lists_ms[4], reuse_ms[4], light_ms[4], full_ms[4], total_ms[4];
    uint32_t tiles[4][3];
} te_sched_observation;

void te_scheduler_observe(te_scheduler* s, const te_sched_observation* o, double budget_ms);

#ifdef __cplusplus
}
#endif

#endif

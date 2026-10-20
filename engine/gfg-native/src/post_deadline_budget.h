/* SPDX-License-Identifier: GPL-3.0-or-later
 * Pure budget for a separately owned post-game output surface.
 * It NEVER fabricates GPU readiness or output presentation. The owner
 * must check the real fence and use the returned synthetic count only
 * before acquiring the downstream WSI image.
 */
#ifndef GFG_POST_DEADLINE_BUDGET_H
#define GFG_POST_DEADLINE_BUDGET_H
#include <stdint.h>
typedef struct {
    uint64_t refresh_tick_ns;
    uint64_t max_real_age_ns;
    uint64_t real_present_cost_ns;
    uint64_t synthetic_cost_ns;
} gfg_post_deadline_budget;

static inline uint64_t gfg_post_saturating_add(uint64_t a,uint64_t b) {
    return UINT64_MAX-a<b?UINT64_MAX:a+b;
}
/* Count optional companions affordable after all pending output work,
 * while reserving a full display tick and present cost for real B.
 * Age is based on ORIGINAL CAPTURE, never rebased to GPU completion.
 * If the budget is exceeded, return zero: the real endpoint is still
 * mandatory, and MUST be submitted by the caller.
 *
 * A queue at 100% load cannot create FPS by absorbing backlog.  Unlike
 * a proportional queue formula, this decision is monotone decreasing
 * as source age, pending output debt or GPU work cost rises.
 */
static inline uint32_t gfg_post_admit_companions(
        const gfg_post_deadline_budget *b,
        uint64_t captured_ns,uint64_t now_ns,
        uint64_t outstanding_output_ns,uint32_t requested) {
    if(!b||!captured_ns||now_ns<captured_ns||
       !b->refresh_tick_ns||!b->max_real_age_ns)return 0;
    if(requested>3u)requested=3u;
    const uint64_t age=gfg_post_saturating_add(
        now_ns-captured_ns,outstanding_output_ns);
    const uint64_t real_reserve=gfg_post_saturating_add(
        b->refresh_tick_ns,b->real_present_cost_ns);
    if(age>=b->max_real_age_ns ||
       real_reserve>=b->max_real_age_ns-age)return 0;
    const uint64_t remaining=b->max_real_age_ns-age-real_reserve;
    const uint64_t cost=b->synthetic_cost_ns>b->refresh_tick_ns?
        b->synthetic_cost_ns:b->refresh_tick_ns;
    const uint64_t affordable=remaining/cost;
    return affordable<(uint64_t)requested?(uint32_t)affordable:requested;
}
#endif

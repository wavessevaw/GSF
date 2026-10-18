/* SPDX-License-Identifier: GPL-3.0-or-later
 * Present-ID remapping logic (no Vulkan driver involved).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/present_ids.h"

int main(void) {
    static gfg_present_map m;
    memset(&m, 0, sizeof m);
    uint64_t l = 0;
    assert(!gfg_pm_translate(&m, 1, &l));          /* nothing presented yet */
    /* frame 0: R only; frames 1..: G1, G2, R */
    uint64_t r0 = gfg_pm_next(&m);
    gfg_pm_map(&m, 10, r0);
    uint64_t g1 = gfg_pm_next(&m), g2 = gfg_pm_next(&m), r1 = gfg_pm_next(&m);
    gfg_pm_map(&m, 20, r1);
    assert(r0 == 1 && g1 == 2 && g2 == 3 && r1 == 4); /* strictly increasing */
    assert(gfg_pm_translate(&m, 10, &l) && l == r0);
    assert(gfg_pm_translate(&m, 5, &l) && l == r0);   /* skipped app ids */
    assert(gfg_pm_translate(&m, 11, &l) && l == r1);  /* first app id >= 11 */
    assert(gfg_pm_translate(&m, 20, &l) && l == r1);
    assert(!gfg_pm_translate(&m, 21, &l));            /* future: caller must wait */
    gfg_pm_map(&m, 20, gfg_pm_next(&m));              /* non-increasing app id: ignored */
    assert(m.last_app == 20 && m.count == 2);
    /* ring wrap: old ids resolve to the oldest remembered real present */
    for (uint64_t a = 21; a < 21 + 2 * GFG_PRESENT_MAP_RING; ++a) {
        gfg_pm_next(&m);
        gfg_pm_next(&m);
        gfg_pm_map(&m, a, gfg_pm_next(&m));
    }
    assert(m.count == GFG_PRESENT_MAP_RING);
    assert(gfg_pm_translate(&m, 1, &l) && l == m.layer[m.head]);
    assert(gfg_pm_translate(&m, m.last_app, &l) && l == m.last_layer);
    puts("GFG native: present-id remapping logic PASS");
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * Per-swapchain present-ID remapping (review finding B4).
 *
 * With frame generation the layer presents G1, G2, R per real frame. Present
 * IDs must be strictly increasing per swapchain, so the layer owns the ID
 * sequence and gives every present (synthetic or real) a fresh layer ID.
 * The app's own IDs (carried by its real presents) are remembered as
 * app_id -> layer_id so vkWaitForPresent(2)KHR(app_id) can be translated:
 * waiting for app id X means "a present with app id >= X completed"; in layer
 * space that is the layer id of the first real present whose app id >= X.
 * Pure logic, no Vulkan calls: unit-tested in tests/present_ids.c.
 */
#ifndef GFG_NATIVE_PRESENT_IDS_H
#define GFG_NATIVE_PRESENT_IDS_H
#include <stdint.h>

#define GFG_PRESENT_MAP_RING 1024u

typedef struct {
    uint64_t last_layer;                  /* last layer id handed out */
    uint64_t last_app;                    /* last app id mapped (0: none) */
    uint64_t app[GFG_PRESENT_MAP_RING];   /* increasing app ids, oldest at head */
    uint64_t layer[GFG_PRESENT_MAP_RING];
    uint32_t head, count;
} gfg_present_map;

/* A fresh, strictly increasing layer id. */
static inline uint64_t gfg_pm_next(gfg_present_map *m) { return ++m->last_layer; }

/* Record that the real present with app id `app_id` was given `layer_id`.
 * Ids that do not increase (invalid app usage) are not mapped. */
static inline void gfg_pm_map(gfg_present_map *m, uint64_t app_id, uint64_t layer_id) {
    if (!app_id || app_id <= m->last_app) return;
    uint32_t slot;
    if (m->count < GFG_PRESENT_MAP_RING) {
        slot = (m->head + m->count) % GFG_PRESENT_MAP_RING;
        ++m->count;
    } else {
        slot = m->head;
        m->head = (m->head + 1) % GFG_PRESENT_MAP_RING;
    }
    m->app[slot] = app_id;
    m->layer[slot] = layer_id;
    m->last_app = app_id;
}

/* 1 and *layer_id when app id X can be translated now; 0 when no real present
 * with app id >= X has been submitted yet (the caller waits for one). */
static inline int gfg_pm_translate(const gfg_present_map *m, uint64_t app_id, uint64_t *layer_id) {
    if (!m->count || app_id > m->last_app) return 0;
    /* ids older than the ring were presented before its oldest entry */
    uint64_t found = m->layer[m->head];
    for (uint32_t i = 0; i < m->count; ++i) {
        uint32_t s = (m->head + i) % GFG_PRESENT_MAP_RING;
        if (m->app[s] >= app_id) { found = m->layer[s]; break; }
    }
    *layer_id = found;
    return 1;
}
#endif

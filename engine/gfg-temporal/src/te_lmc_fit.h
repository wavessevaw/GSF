/* SPDX-License-Identifier: GPL-3.0-or-later */
/* LMC motion model fitted on the CPU (te_lmc_fit.c). */
#ifndef TE_LMC_FIT_H
#define TE_LMC_FIT_H

#include <stdint.h>

#define TE_LMC_FIT_LAYERS 3
#define TE_LMC_FIT_PALETTE 4
#define TE_LMC_FIT_ITERS 128
#define TE_LMC_FIT_SCORE 512   /* blocks a hypothesis is ranked on */
#define TE_LMC_FIT_IRLS 2048   /* inliers the IRLS refines on */
#define TE_LMC_MIN_SHARE 0.10

typedef struct {
    float h[3][3];   /* A -> B, full-res px, h[2][2] = 1 */
    float share;     /* explained share of the usable blocks */
} te_lmc_plane;

typedef struct {
    uint32_t nlayers;
    te_lmc_plane layers[TE_LMC_FIT_LAYERS];
    uint32_t npal;
    float pal[TE_LMC_FIT_PALETTE][2];   /* full-res px */
} te_lmc_model;

typedef struct te_lmc_fit_work te_lmc_fit_work;   /* ~3 MB of scratch, one per context */
te_lmc_fit_work* te_lmc_fit_alloc(void);
void te_lmc_fit_free(te_lmc_fit_work* w);

/* vec: bw*bh level-1 B-grid block vectors in full-res px (x, y); usable: the
 * fit's block mask; hudb: HUD blocks (left out of the palette).  sub thins the
 * fit to every sub-th block in each direction.  seed: the pair counter; prev:
 * the previous model, its planes are tried as hypotheses.  0 on success. */
int te_lmc_fit(te_lmc_fit_work* work, const float* vec, const unsigned char* usable, const unsigned char* hudb, int bw, int bh, int sub,
               uint32_t seed, const te_lmc_model* prev, te_lmc_model* out);

#endif

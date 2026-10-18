/* SPDX-License-Identifier: GPL-3.0-or-later */
/* LMC motion model on the CPU: camera planes (RANSAC + Tukey IRLS homographies)
 * and the motion palette, fitted on this pair's block vectors between the two
 * GPU submissions. Reference: bench/gen_lmc2.py fit_layers, ransac_plane,
 * irls_h, fit_h_ls, hist_median, motion_palette.
 *
 * The GPU did this in single-workgroup passes that needed a whole compute unit
 * free; with a game on the GPU such a unit rarely is, and the passes waited for
 * milliseconds.  Here it is a few hundred microseconds of one CPU core. */
#include "te_lmc_fit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PTS 32768

static uint32_t pcg(uint32_t* state) {
    *state = *state * 747796405u + 2891336453u;
    uint32_t word = ((*state >> ((*state >> 28u) + 4u)) ^ *state) * 277803737u;
    return (word >> 22u) ^ word;
}

static inline double dist2d(double x, double y) { return sqrt(x * x + y * y); }

static void apply_h(const double H[3][3], double x, double y, double* ox, double* oy) {
    double d = H[2][0] * x + H[2][1] * y + H[2][2];
    *ox = (H[0][0] * x + H[0][1] * y + H[0][2]) / d;
    *oy = (H[1][0] * x + H[1][1] * y + H[1][2]) / d;
}

/* 8x8 solve with partial pivoting (numpy.linalg.solve); 0 when singular */
static int solve8(double M[8][9], double h[8]) {
    for (int c = 0; c < 8; ++c) {
        int piv = c;
        for (int r = c + 1; r < 8; ++r)
            if (fabs(M[r][c]) > fabs(M[piv][c])) piv = r;
        if (fabs(M[piv][c]) < 1e-300) return 0;
        if (piv != c)
            for (int k = 0; k < 9; ++k) { double t = M[c][k]; M[c][k] = M[piv][k]; M[piv][k] = t; }
        for (int r = c + 1; r < 8; ++r) {
            double f = M[r][c] / M[c][c];
            for (int k = c; k < 9; ++k) M[r][k] -= f * M[c][k];
        }
    }
    for (int r = 7; r >= 0; --r) {
        double s = M[r][8];
        for (int k = r + 1; k < 8; ++k) s -= M[r][k] * h[k];
        h[r] = s / M[r][r];
    }
    return 1;
}

/* Weighted homography with h33 = 1 on Hartley-normalised points (fit_h_ls) */
static int fit_h_ls_idx(int n, const int* ix, const double* sx, const double* sy, const double* dx, const double* dy,
                        const double* w, double H[3][3]);

static int fit_h_ls(int n, const double* sx, const double* sy, const double* dx, const double* dy,
                    const double* w, double H[3][3]) {
    return fit_h_ls_idx(n, NULL, sx, sy, dx, dy, w, H);
}

/* over points ix[0..n) (all of 0..n when ix is NULL) */
static int fit_h_ls_idx(int n, const int* ix, const double* sx, const double* sy, const double* dx, const double* dy,
                        const double* w, double H[3][3]) {
    double W = 0, csx = 0, csy = 0, cdx = 0, cdy = 0;
    for (int j = 0; j < n; ++j) {
        int i = ix ? ix[j] : j;
        W += w[i];
        csx += w[i] * sx[i]; csy += w[i] * sy[i];
        cdx += w[i] * dx[i]; cdy += w[i] * dy[i];
    }
    W = W > 1e-9 ? W : 1e-9;
    csx /= W; csy /= W; cdx /= W; cdy /= W;
    double ds = 0, dd = 0;
    for (int j = 0; j < n; ++j) {
        int i = ix ? ix[j] : j;
        ds += w[i] * dist2d(sx[i] - csx, sy[i] - csy);
        dd += w[i] * dist2d(dx[i] - cdx, dy[i] - cdy);
    }
    double ss = sqrt(2.0) / fmax(ds / W, 1e-6), sd = sqrt(2.0) / fmax(dd / W, 1e-6);
    double M[8][9];
    memset(M, 0, sizeof M);
    for (int j = 0; j < n; ++j) {
        int i = ix ? ix[j] : j;
        if (w[i] <= 0) continue;
        double x = (sx[i] - csx) * ss, y = (sy[i] - csy) * ss;
        double u = (dx[i] - cdx) * sd, v = (dy[i] - cdy) * sd;
        double r0[8] = {x, y, 1, 0, 0, 0, -u * x, -u * y};
        double r1[8] = {0, 0, 0, x, y, 1, -v * x, -v * y};
        /* The x and y equations have disjoint affine columns. Skip their
         * structurally zero products, retaining the original expression and
         * accumulation order wherever both equations contribute. This matters
         * in the six IRLS solves on every pair, not only in RANSAC. */
        for (int a = 0; a < 3; ++a) {
            for (int b = a; b < 3; ++b) M[a][b] += w[i] * (r0[a] * r0[b]);
            for (int b = 6; b < 8; ++b) M[a][b] += w[i] * (r0[a] * r0[b]);
            M[a][8] += w[i] * (r0[a] * u);
        }
        for (int a = 3; a < 6; ++a) {
            for (int b = a; b < 6; ++b) M[a][b] += w[i] * (r1[a] * r1[b]);
            for (int b = 6; b < 8; ++b) M[a][b] += w[i] * (r1[a] * r1[b]);
            M[a][8] += w[i] * (r1[a] * v);
        }
        for (int a = 6; a < 8; ++a) {
            for (int b = a; b < 8; ++b) M[a][b] += w[i] * (r0[a] * r0[b] + r1[a] * r1[b]);
            M[a][8] += w[i] * (r0[a] * u + r1[a] * v);
        }
    }
    for (int a = 0; a < 8; ++a) {
        for (int b = 0; b < a; ++b) M[a][b] = M[b][a];
        M[a][a] += 1e-9;
    }
    double h[8];
    if (!solve8(M, h)) return 0;
    /* H = Td^-1 * Hn * Ts */
    double Hn[3][3] = {{h[0], h[1], h[2]}, {h[3], h[4], h[5]}, {h[6], h[7], 1}};
    double Ts[3][3] = {{ss, 0, -ss * csx}, {0, ss, -ss * csy}, {0, 0, 1}};
    double Tdi[3][3] = {{1 / sd, 0, cdx}, {0, 1 / sd, cdy}, {0, 0, 1}};
    double T[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            T[r][c] = 0;
            for (int k = 0; k < 3; ++k) T[r][c] += Hn[r][k] * Ts[k][c];
        }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            H[r][c] = 0;
            for (int k = 0; k < 3; ++k) H[r][c] += Tdi[r][k] * T[k][c];
        }
    double s = H[2][2];
    if (!isfinite(s) || fabs(s) < 1e-300) return 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) H[r][c] /= s;
    return 1;
}


typedef struct {
    int n;
    double sx[MAX_PTS], sy[MAX_PTS], dx[MAX_PTS], dy[MAX_PTS], tol[MAX_PTS], w0[MAX_PTS];
    double wref[MAX_PTS], w[MAX_PTS], r[MAX_PTS];
    int idx[MAX_PTS];
} fit_work;

/* Tukey IRLS (irls_h) over the blocks with wref > 0 only: the others carry no weight */
static int irls(fit_work* f, double H[3][3]) {
    int nz = 0;
    int m = 0;
    for (int i = 0; i < f->n; ++i) {
        f->w[i] = f->wref[i];
        m += f->wref[i] > 0;
    }
    if (m < 8) return 0;
    /* an evenly strided subset of at most TE_LMC_FIT_IRLS inliers: the plane is
       as precise, the fit a fraction of the cost */
    int stride = (m + TE_LMC_FIT_IRLS - 1) / TE_LMC_FIT_IRLS, seen = 0;
    for (int i = 0; i < f->n; ++i)
        if (f->wref[i] > 0 && seen++ % stride == 0) f->idx[nz++] = i;
    for (int it = 0; it < 6; ++it) {
        if (!fit_h_ls_idx(nz, f->idx, f->sx, f->sy, f->dx, f->dy, f->w, H)) return 0;
        uint32_t hist[256] = {0};
        for (int j = 0; j < nz; ++j) {
            int i = f->idx[j];
            double px, py;
            apply_h(H, f->sx[i], f->sy[i], &px, &py);
            f->r[i] = dist2d(px - f->dx[i], py - f->dy[i]);
            double b = f->r[i] / 0.25;
            hist[b > 255 ? 255 : (int)b]++;
        }
        uint32_t half = (uint32_t)(nz + 1) / 2, cum = 0;
        int k = 0;
        for (; k < 255; ++k) {
            cum += hist[k];
            if (cum >= half) break;
        }
        double c = fmax(2.0, 2.5 * (k + 0.5) * 0.25);
        for (int j = 0; j < nz; ++j) {
            int i = f->idx[j];
            double u = f->r[i] / c;
            f->w[i] = f->r[i] < c ? (1 - u * u) * (1 - u * u) : 0.0;   /* wref is 1 here */
        }
    }
    return 1;
}

static int finite_h(const double H[3][3]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (!isfinite(H[r][c])) return 0;
    return 1;
}

/* ransac_plane: 1 when a plane was fitted; explained[] marks its blocks */
static int ransac_plane(fit_work* f, uint32_t seed, const double (*seeds)[3][3], int nseeds, double H[3][3],
                        unsigned char* explained) {
    int m = 0;
    for (int i = 0; i < f->n; ++i)
        if (f->w0[i] > 0) f->idx[m++] = i;
    if (m < 12) return 0;
    /* Rank in the original order, but construct hypotheses only when needed.
     * A hypothesis cannot beat best_n after its misses exhaust that margin.
     * This is an exact upper bound, not an approximate early acceptance: ties
     * still keep the first model, and IRLS/inlier classification are unchanged.
     * Smooth pans often have a perfect previous-model seed, in which case no
     * random solves are needed at all. */
    int stride = (m + TE_LMC_FIT_SCORE - 1) / TE_LMC_FIT_SCORE;
    int scored = (m + stride - 1) / stride;
    int best_n = 0, have_best = 0;
    double best_h[3][3] = {{0}};
    for (int candidate = 0; candidate < nseeds + TE_LMC_FIT_ITERS; ++candidate) {
        double hyp[3][3];
        if (best_n == scored) break;
        if (candidate < nseeds) {
            memcpy(hyp, seeds[candidate], sizeof hyp);
        } else {
            int it = candidate - nseeds;
            uint32_t st = seed * 9781u + (uint32_t)it * 6271u + 1u;
            int pick[4];
            for (int k = 0; k < 4; ++k) pick[k] = f->idx[pcg(&st) % (uint32_t)m];
            int ok = 1;
            for (int a = 0; a < 4 && ok; ++a)
                for (int b = a + 1; b < 4; ++b)
                    if (dist2d(f->sx[pick[a]] - f->sx[pick[b]], f->sy[pick[a]] - f->sy[pick[b]]) < 120.0) { ok = 0; break; }
            if (!ok) continue;
            double sx[4], sy[4], dx[4], dy[4], w[4] = {1, 1, 1, 1};
            for (int k = 0; k < 4; ++k) {
                sx[k] = f->sx[pick[k]]; sy[k] = f->sy[pick[k]];
                dx[k] = f->dx[pick[k]]; dy[k] = f->dy[pick[k]];
            }
            if (!fit_h_ls(4, sx, sy, dx, dy, w, hyp)) continue;
        }
        if (!finite_h(hyp)) continue;
        int cnt = 0, remaining = scored;
        for (int j = 0; j < m; j += stride) {
            int i = f->idx[j];
            double px, py;
            apply_h(hyp, f->sx[i], f->sy[i], &px, &py);
            double ex = px - f->dx[i], ey = py - f->dy[i];
            if (ex * ex + ey * ey < f->tol[i] * f->tol[i]) cnt++;
            --remaining;
            if (cnt + remaining <= best_n) break;
        }
        if (cnt > best_n) {
            best_n = cnt;
            have_best = 1;
            memcpy(best_h, hyp, sizeof best_h);
        }
    }
    if (!have_best || best_n * stride < 8) return 0;
    for (int i = 0; i < f->n; ++i) {
        f->wref[i] = 0;
        if (f->w0[i] <= 0) continue;
        double px, py;
        apply_h(best_h, f->sx[i], f->sy[i], &px, &py);
        if (dist2d(px - f->dx[i], py - f->dy[i]) < f->tol[i]) f->wref[i] = 1.0;
    }
    if (!irls(f, H) || !finite_h(H)) return 0;
    for (int i = 0; i < f->n; ++i) {
        double px, py;
        apply_h(H, f->sx[i], f->sy[i], &px, &py);
        explained[i] = f->w0[i] > 0 && dist2d(px - f->dx[i], py - f->dy[i]) < f->tol[i];
    }
    return 1;
}

struct te_lmc_fit_work {
    fit_work f;
    unsigned char explained[MAX_PTS];
    unsigned char live[MAX_PTS];
    uint32_t hist[64 * 64];
    uint16_t q[MAX_PTS];
};

te_lmc_fit_work* te_lmc_fit_alloc(void) { return calloc(1, sizeof(te_lmc_fit_work)); }
void te_lmc_fit_free(te_lmc_fit_work* w) { free(w); }

static void palette(te_lmc_fit_work* wk, const float* vec, const unsigned char* hudb, int n, te_lmc_model* out) {
    unsigned char* live = wk->live;
    uint32_t* hist = wk->hist;
    uint16_t* q = wk->q;   /* 8 px bin of every block (64 x 64 grid), computed once */
    int nl = 0;
    for (int i = 0; i < n; ++i) {
        live[i] = !hudb[i];
        nl += live[i];
        int qx = (int)floorf((vec[2 * i] + 256.0f) / 8.0f), qy = (int)floorf((vec[2 * i + 1] + 256.0f) / 8.0f);
        qx = qx < 0 ? 0 : qx > 63 ? 63 : qx;
        qy = qy < 0 ? 0 : qy > 63 ? 63 : qy;
        q[i] = (uint16_t)(qy * 64 + qx);
    }
    double total = nl > 1 ? nl : 1;
    out->npal = 0;
    for (int k = 0; k < TE_LMC_FIT_PALETTE; ++k) {
        memset(hist, 0, sizeof wk->hist);
        for (int i = 0; i < n; ++i)
            if (live[i]) hist[q[i]]++;
        int b = 0;
        for (int j = 1; j < 64 * 64; ++j)
            if (hist[j] > hist[b]) b = j;   /* first maximum (numpy argmax) */
        if (hist[b] < 0.01 * total) break;
        int bx = b % 64, by = b / 64;
        uint32_t fh[17 * 17] = {0};
        for (int i = 0; i < n; ++i) {
            if (!live[i] || q[i] != b) continue;
            int fx = (int)floorf((vec[2 * i] - (bx * 8.0f - 256.0f)) / 0.5f + 0.5f);
            int fy = (int)floorf((vec[2 * i + 1] - (by * 8.0f - 256.0f)) / 0.5f + 0.5f);
            fx = fx < 0 ? 0 : fx > 16 ? 16 : fx;
            fy = fy < 0 ? 0 : fy > 16 ? 16 : fy;
            fh[fy * 17 + fx]++;
        }
        int fb = 0;
        for (int j = 1; j < 17 * 17; ++j)
            if (fh[j] > fh[fb]) fb = j;
        float mx = (float)(bx * 8.0 - 256.0 + (fb % 17) * 0.5), my = (float)(by * 8.0 - 256.0 + (fb / 17) * 0.5);
        out->pal[k][0] = mx;
        out->pal[k][1] = my;
        out->npal = (uint32_t)(k + 1);
        for (int i = 0; i < n; ++i) {
            float ax = vec[2 * i] - mx, ay = vec[2 * i + 1] - my;
            if ((ax < 0 ? -ax : ax) <= 1.5f && (ay < 0 ? -ay : ay) <= 1.5f) live[i] = 0;
        }
    }
}

int te_lmc_fit(te_lmc_fit_work* wk, const float* vec, const unsigned char* usable, const unsigned char* hudb, int bw,
               int bh, int sub, uint32_t seed, const te_lmc_model* prev, te_lmc_model* out) {
    fit_work* fp = &wk->f;
    unsigned char* explained = wk->explained;
    int n = bw * bh;
    memset(out, 0, sizeof *out);
    if (n <= 0 || n > MAX_PTS) return -1;
    sub = sub < 1 ? 1 : sub;
    fp->n = n;
    double total = 0;
    for (int i = 0; i < n; ++i) {
        int bx = i % bw, by = i / bw;
        double cx = bx * 8.0 + 3.5, cy = by * 8.0 + 3.5;
        double vx = vec[2 * i], vy = vec[2 * i + 1];
       fp->sx[i] = cx - vx;
       fp->sy[i] = cy - vy;
       fp->dx[i] = cx;
       fp->dy[i] = cy;
       fp->tol[i] = 1.5 + 0.03 * dist2d(vx, vy);
       fp->w0[i] = (usable[i] && bx % sub == 0 && by % sub == 0) ? 1.0 : 0.0;
        total +=fp->w0[i];
    }
    total = total > 1 ? total : 1;
    double seeds[TE_LMC_FIT_LAYERS][3][3];
    int nseeds = 0;
    if (prev)
        for (uint32_t k = 0; k < prev->nlayers && k < TE_LMC_FIT_LAYERS; ++k) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) seeds[nseeds][r][c] = prev->layers[k].h[r][c];
            nseeds++;
        }
    for (int L = 0; L < TE_LMC_FIT_LAYERS; ++L) {
        double H[3][3];
        if (!ransac_plane(fp, seed * 4u + (uint32_t)out->nlayers, (const double(*)[3][3])seeds, nseeds, H, explained))
            break;
        double ex = 0;
        for (int i = 0; i < n; ++i) ex += explained[i];
        double share = ex / total;
        if (share < TE_LMC_MIN_SHARE) break;
        te_lmc_plane* p = &out->layers[out->nlayers++];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) p->h[r][c] = (float)H[r][c];
        p->share = (float)share;
        for (int i = 0; i < n; ++i)
            if (explained[i])fp->w0[i] = 0;
    }
    palette(wk, vec, hudb, n, out);
    return 0;
}

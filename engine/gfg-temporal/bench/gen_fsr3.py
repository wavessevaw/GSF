"""FSR 3 frame interpolation without game motion vectors, reimplemented from the
FidelityFX SDK shaders (MIT) for the bench:

  optical flow  ffx_opticalflow_compute_optical_flow_v5.h: 7-level luma pyramid,
                8x8 blocks, SAD full search +-8 around the vector predicted from
                the coarser level (scale pass: best of 4 parents by SAD),
                3x3 vector-median filter per level (filter_optical_flow_v5.h).
  vector field  ffx_frameinterpolation_optical_flow_vector_field.h: coherence-
                weighted 3x3 average, then forward splat of the vector to its
                position at time t; collisions keep the faster vector (velocity
                priority), then the one whose two ends match in luma.
  interpolation ffx_frameinterpolation.h: two-sided bilinear fetch along the
                splatted vector (one side only where the other leaves the screen).

FSR 3 makes one frame at t = 0.5; here the same steps are evaluated at each t.
Luma is the perceptual L* of linear Rec.709 luminance, as in prepare_luma.h.
"""
from __future__ import annotations

import numpy as np

from methods import register

BLOCK = 8
RADIUS = 8
LEVELS = 7


def perceived_luma(rgb):
    lin = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    y = lin[..., 0] * 0.2126 + lin[..., 1] * 0.7152 + lin[..., 2] * 0.0722
    l = np.where(y <= 216.0 / 24389.0, y * (24389.0 / 27.0), np.cbrt(y) * 116.0 - 16.0) * 0.01
    return np.clip(l * 255.0, 0, 255).astype(np.float32)


def down(y):
    h, w = (y.shape[0] + 1) // 2 * 2, (y.shape[1] + 1) // 2 * 2
    p = np.pad(y, ((0, h - y.shape[0]), (0, w - y.shape[1])), mode="edge")
    return 0.25 * (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2])


def block_coords(bh, bw):
    by, bx = np.meshgrid(np.arange(bh), np.arange(bw), indexing="ij")
    oy, ox = np.meshgrid(np.arange(BLOCK), np.arange(BLOCK), indexing="ij")
    ys = by[..., None, None] * BLOCK + oy
    xs = bx[..., None, None] * BLOCK + ox
    return ys, xs  # (bh, bw, 8, 8)


def sad_at(cur, prev, ys, xs, v):
    """SAD of each current block against prev displaced by integer v (bh, bw, 2)."""
    h, w = cur.shape
    c = cur[np.minimum(ys, h - 1), np.minimum(xs, w - 1)]
    py = np.clip(ys + v[..., 1, None, None], 0, h - 1)
    px = np.clip(xs + v[..., 0, None, None], 0, w - 1)
    return np.abs(c - prev[py, px]).sum(axis=(2, 3))


def vector_median(v):
    bh, bw, _ = v.shape
    p = np.pad(v, ((1, 1), (1, 1), (0, 0)), mode="edge")
    nb = np.stack([p[1 + dy:1 + dy + bh, 1 + dx:1 + dx + bw] for dx in (-1, 0, 1) for dy in (-1, 0, 1)], 0)
    d = ((nb[:, None] - nb[None, :]) ** 2).sum(-1).sum(1)  # (9, bh, bw)
    i = np.argmin(d, axis=0)
    return np.take_along_axis(nb, i[None, ..., None], 0)[0]


def optical_flow(cur_l, prev_l):
    """Integer block vectors (pixels) at level 0, block 8: cur(p) ~ prev(p + v)."""
    pc, pp = [cur_l], [prev_l]
    for _ in range(LEVELS - 1):
        pc.append(down(pc[-1]))
        pp.append(down(pp[-1]))
    v = None
    for level in range(LEVELS - 1, -1, -1):
        cur, prev = pc[level], pp[level]
        bh, bw = -(-cur.shape[0] // BLOCK), -(-cur.shape[1] // BLOCK)
        ys, xs = block_coords(bh, bw)
        if v is None:
            pred = np.zeros((bh, bw, 2), np.int32)
        else:
            # scale pass: each block picks the best of its 4 nearest parents (x2)
            cands = []
            for oy in (0, 1):
                for ox in (0, 1):
                    iy = np.clip((np.arange(bh) - 1 + oy + (np.arange(bh) % 2)) // 2, 0, v.shape[0] - 1)
                    ix = np.clip((np.arange(bw) - 1 + ox + (np.arange(bw) % 2)) // 2, 0, v.shape[1] - 1)
                    cands.append(2 * v[iy][:, ix])
            costs = np.stack([sad_at(cur, prev, ys, xs, c) for c in cands])
            pred = np.take_along_axis(np.stack(cands), np.argmin(costs, 0)[None, ..., None], 0)[0]
        best = np.full((bh, bw), np.inf, np.float32)
        bestv = pred.copy()
        # search order mirrors the shader's packed-min tie break: smaller
        # |offset - centre| wins on equal SAD (FIX_TOP_LEFT_BIAS)
        offs = sorted(((dx, dy) for dy in range(-RADIUS, RADIUS) for dx in range(-RADIUS, RADIUS)),
                      key=lambda o: (abs(o[1]), abs(o[0])))
        for dx, dy in offs:
            c = pred + np.array([dx, dy], np.int32)
            s = sad_at(cur, prev, ys, xs, c)
            better = s < best
            best = np.where(better, s, best)
            bestv = np.where(better[..., None], c, bestv)
        if level == 0:  # local search fallback: keep zero if it is as good
            z = sad_at(cur, prev, ys, xs, np.zeros_like(bestv))
            bestv = np.where((z <= best)[..., None], 0, bestv)
        v = vector_median(bestv)
    return v.astype(np.float32)


def bilinear(img, x, y):
    if img.ndim == 2:
        return bilinear(img[..., None], x, y)[..., 0]
    h, w = img.shape[:2]
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0, y0 = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    a = img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx
    b = img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy


def smooth_field(v, size):
    """3x3 coherence-weighted average of block vectors (vector_field pass)."""
    bh, bw, _ = v.shape
    uv = v / np.array(size, np.float32)  # the shader works in UV units
    p = np.pad(uv, ((1, 1), (1, 1), (0, 0)), mode="edge")
    nb = np.stack([p[1 + dy:1 + dy + bh, 1 + dx:1 + dx + bw] for dy in (-1, 0, 1) for dx in (-1, 0, 1)], 0)
    ln = np.linalg.norm(nb * np.array(size, np.float32), axis=-1)
    lf = np.maximum(0, 512 - ln) * (ln > 1)
    avg = (nb * lf[..., None]).sum(0) / np.maximum(lf.sum(0), 1e-9)[..., None]
    w = np.maximum(0, (nb * avg[None]).sum(-1)) ** 1.25 * lf
    sw = w.sum(0)
    out = np.where((sw > 1e-12)[..., None], (nb * w[..., None]).sum(0) / np.maximum(sw, 1e-30)[..., None], uv)
    return out * np.array(size, np.float32)


@register("fsr3")
class FSR3:
    def __init__(self, w, h):
        self.prev = None
        self.size = (w, h)

    def push(self, frame, ts):
        cur = frame.astype(np.float32) / 255.0
        cl = perceived_luma(cur)
        prev, self.prev = self.prev, (cur, cl)
        if prev is None:
            return []
        a, al = prev
        v = optical_flow(cl, al)               # cur(p) = prev(p + v)
        v = smooth_field(v, self.size)
        return [self._interp(a, cur, al, cl, v, t) for t in ts]

    def _interp(self, a, b, al, bl, v, t):
        w, h = self.size
        bh, bw, _ = v.shape
        # splat each block vector to its position at time t (block centre in b)
        cy, cx = np.meshgrid((np.arange(bh) + 0.5) * BLOCK, (np.arange(bw) + 0.5) * BLOCK, indexing="ij")
        vel = np.linalg.norm(v, axis=-1)
        hi = np.where(vel > 1.0, np.clip(vel / (np.hypot(w, h) * 0.05), 0, 1) * 255, 0).astype(np.int64)
        pa = bilinear(al, cx + v[..., 0], cy + v[..., 1]) + 0.255
        pb = bl[np.minimum(cy.astype(int), h - 1), np.minimum(cx.astype(int), w - 1)] + 0.255
        inside = (cx + v[..., 0] >= 0) & (cx + v[..., 0] < w) & (cy + v[..., 1] >= 0) & (cy + v[..., 1] < h)
        lo = (np.round(np.minimum(pa, pb) / np.maximum(pa, pb) * 255) * inside).astype(np.int64)
        prio = hi * 256 + lo
        tx = (cx + (1 - t) * v[..., 0]) / BLOCK - 0.5
        ty = (cy + (1 - t) * v[..., 1]) / BLOCK - 0.5
        field = np.zeros((bh, bw, 2), np.float32)
        best = np.zeros((bh, bw), np.int64)
        keep = hi > 0
        x0, y0 = np.floor(tx).astype(int), np.floor(ty).astype(int)
        for oy in (0, 1):
            for ox in (0, 1):
                X, Y = x0 + ox, y0 + oy
                ok = keep & (X >= 0) & (X < bw) & (Y >= 0) & (Y < bh)
                order = np.argsort(prio[ok], kind="stable")  # ascending: max wins last
                Xs, Ys, P, V = X[ok][order], Y[ok][order], prio[ok][order], v[ok][order]
                upd = P > best[Ys, Xs]
                # sequential atomic-max emulation (later, higher priority overwrites)
                for xi, yi, pi, vi in zip(Xs[upd], Ys[upd], P[upd], V[upd]):
                    if pi > best[yi, xi]:
                        best[yi, xi] = pi
                        field[yi, xi] = vi
        # per-pixel field: bilinear of the splatted cells (empty cells are zero)
        py, px = np.meshgrid(np.arange(h, dtype=np.float32), np.arange(w, dtype=np.float32), indexing="ij")
        fv = bilinear(field, (px + 0.5) / BLOCK - 0.5, (py + 0.5) / BLOCK - 0.5)
        ax, ay = px + t * fv[..., 0], py + t * fv[..., 1]
        bx, by = px - (1 - t) * fv[..., 0], py - (1 - t) * fv[..., 1]
        ca, cb = bilinear(a, ax, ay), bilinear(b, bx, by)
        ina = ((ax >= 0) & (ax < w) & (ay >= 0) & (ay < h))[..., None]
        inb = ((bx >= 0) & (bx < w) & (by >= 0) & (by < h))[..., None]
        of = np.where(ina & inb, ca * (1 - t) + cb * t, np.where(ina, ca, cb))
        # With no game vectors the shader's final step (lean towards the
        # co-located "game" blend when it looks more similar) degenerates into a
        # plain blend, because colour cosine similarity is ~1 almost everywhere.
        # MV-free ports (lsfg-vk-afmf, OptiFG without inputs) use the optical
        # flow colour itself, so the bench does too.
        return of

"""LMC: layered motion compensation (candidate for the next Temporal Engine).

Motion in a game frame is layered, so each layer is solved by the cheapest
method that is exact for it:

  1. HUD      pixels that stay identical for several pairs while the scene moves
              are never warped: they are copied from the newest real frame and
              excluded from motion estimation.
  2. camera   one homography for the whole frame (exact for camera rotation),
              fitted robustly to the block vectors; it is a candidate
              everywhere, so background vectors are sub-pixel and unlimited in
              range, and blocks never smear the camera motion.
  3. objects  predictive block search (3-D recursive search style): a few
              candidates per block (parent level, neighbours, previous pair,
              camera, zero) and a small update, coarse to fine from 1/16 to 1/2
              resolution; a full search only at 1/16.
  4. pixels   every pixel of both real frames picks its vector from the
              neighbouring blocks and the camera by a 3x3 match, so a thin
              object inside a block keeps its own motion.
  5. time t   each output pixel tests candidate trajectories: a trajectory is
              valid on the side where that frame's own pixel moves with it.
              Valid on both sides -> two-sided blend; only in the previous
              frame -> the pixel is being covered, take it from the previous
              frame; only in the newest -> being uncovered, take it from there.
              This replaces te.2's "nearest frame by t" fallback.

Vector convention: v is the motion from frame A (previous) to frame B (newest);
a point at x in A is at x + v in B.
"""
from __future__ import annotations

import os

import numpy as np
from scipy.ndimage import uniform_filter

from methods import register

LEVELS = 4          # pyramid levels below full resolution (1/2 .. 1/16)
BLK = 4             # block side in level pixels (8 px at 1/2 res, 64 px at 1/16)
COARSE_R = 10       # full search radius at 1/16 (+-160 px at full resolution)
HUD_PAIRS = 6       # pairs a pixel must stay still (while the scene moves) to be HUD
STILL = 2.0 / 255
PEN_UPDATE = 0.004
PROPAGATE = 2       # spatial propagation sweeps per level
SMOOTH = 0.002      # cost per level pixel of deviation from the 3x3 median
ORPHAN_BLEND = True
ORPHANS = True
PIX_MARGIN = 0.25
RESID = 0.12        # per-pixel 3x3 cost above which a pixel is "unexplained"
ORPHAN_SHARE = 0.08 # share of unexplained pixels that makes an 8x8 block an orphan
ORPHAN_R = 112      # orphan search radius, full-res pixels
ORPHAN_CAP = int(os.environ.get("GFG_LMC_ORPHAN_CAP", "256"))  # orphan blocks searched per grid (GPU: same env)
ORPHAN_MAX_COST = 0.08   # 3x3 RGB SAD a pixel's other candidates must beat its own block by  # 3DRS-style penalty for update candidates (per level pixel of change)


def luma(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def down(y):
    h, w = y.shape[0] // 2 * 2, y.shape[1] // 2 * 2
    y = y[:h, :w]
    return 0.25 * (y[0::2, 0::2] + y[1::2, 0::2] + y[0::2, 1::2] + y[1::2, 1::2])


def bilinear(img, x, y):
    """Bilinear fetch with edge clamp (flat-index gathers: much faster in numpy)."""
    squeeze = img.ndim == 2
    if squeeze:
        img = img[..., None]
    h, w, c = img.shape
    x = np.clip(x, 0, w - 1.0001).astype(np.float32)
    y = np.clip(y, 0, h - 1.0001).astype(np.float32)
    x0 = x.astype(np.int32)
    y0 = y.astype(np.int32)
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    flat = img.reshape(-1, c)
    i = y0 * w + x0
    p00, p01 = flat[i], flat[i + 1]
    p10, p11 = flat[i + w], flat[i + w + 1]
    out = (p00 + (p01 - p00) * fx) * (1 - fy) + (p10 + (p11 - p10) * fx) * fy
    return out[..., 0] if squeeze else out


def nearest(img, x, y):
    h, w = img.shape[:2]
    xi = np.clip(np.rint(x), 0, w - 1).astype(np.int32)
    yi = np.clip(np.rint(y), 0, h - 1).astype(np.int32)
    return img.reshape(h * w, -1)[yi * w + xi]


def inside(x, y, w, h):
    return (x >= 0) & (x <= w - 1) & (y >= 0) & (y <= h - 1)


# ----------------------------------------------------------------- camera model

def fit_homography(src, dst, weights):
    """Weighted DLT with Hartley normalisation; maps src (A) -> dst (B)."""
    def norm(p):
        c = p.mean(0)
        s = np.sqrt(2) / max(np.mean(np.linalg.norm(p - c, axis=1)), 1e-6)
        T = np.array([[s, 0, -s * c[0]], [0, s, -s * c[1]], [0, 0, 1]])
        return (p - c) * s, T
    ps, Ts = norm(src)
    pd, Td = norm(dst)
    n = len(ps)
    A = np.zeros((2 * n, 9))
    x, y = ps[:, 0], ps[:, 1]
    u, v = pd[:, 0], pd[:, 1]
    A[0::2] = np.stack([-x, -y, -np.ones(n), np.zeros(n), np.zeros(n), np.zeros(n), u * x, u * y, u], 1)
    A[1::2] = np.stack([np.zeros(n), np.zeros(n), np.zeros(n), -x, -y, -np.ones(n), v * x, v * y, v], 1)
    A *= np.repeat(np.sqrt(weights), 2)[:, None]
    _, _, vt = np.linalg.svd(A, full_matrices=False)
    H = vt[-1].reshape(3, 3)
    H = np.linalg.inv(Td) @ H @ Ts
    return H / H[2, 2]


def apply_h(H, x, y):
    d = H[2, 0] * x + H[2, 1] * y + H[2, 2]
    return (H[0, 0] * x + H[0, 1] * y + H[0, 2]) / d, (H[1, 0] * x + H[1, 1] * y + H[1, 2]) / d


def robust_camera(xs, ys, vx, vy, w0, iters=6):
    """IRLS (Tukey) homography from block vectors; returns H and inlier share."""
    src = np.stack([xs, ys], 1)
    dst = src + np.stack([vx, vy], 1)
    w = w0.astype(np.float64).copy()
    if (w > 0).sum() < 12:
        return None, 0.0
    H = None
    for _ in range(iters):
        H = fit_homography(src, dst, w + 1e-9)
        px, py = apply_h(H, xs, ys)
        r = np.hypot(px - dst[:, 0], py - dst[:, 1])
        c = max(2.0, 2.5 * np.median(r[w0 > 0]))
        w = w0 * np.where(r < c, (1 - (r / c) ** 2) ** 2, 0.0)
    inl = float(((r < 1.5 + 0.03 * np.hypot(vx, vy)) & (w0 > 0)).sum() / max((w0 > 0).sum(), 1))
    return H, inl


# ----------------------------------------------------------------- block search

class Level:
    def __init__(self, ya, yb, scale, color_a=None, color_b=None):
        self.ya, self.yb, self.s = ya, yb, scale
        self.h, self.w = ya.shape
        self.bh, self.bw = self.h // BLK, self.w // BLK
        oy, ox = np.meshgrid(np.arange(BLK), np.arange(BLK), indexing="ij")
        by, bx = np.meshgrid(np.arange(self.bh), np.arange(self.bw), indexing="ij")
        self.py = (by[..., None, None] * BLK + oy).astype(np.float32)
        self.px = (bx[..., None, None] * BLK + ox).astype(np.float32)
        self.cy = by * BLK + (BLK - 1) / 2.0
        self.cx = bx * BLK + (BLK - 1) / 2.0
        self.color_a, self.color_b = color_a, color_b

    def cost(self, v, grid):
        """Mean |own - other| per block for vectors v (bh, bw, 2) in level pixels.
        grid 'b': B(x) vs A(x - v); grid 'a': A(x) vs B(x + v)."""
        vx, vy = v[..., 0, None, None], v[..., 1, None, None]
        if grid == "b":
            own = self.yb[self.py.astype(int), self.px.astype(int)]
            oth = bilinear(self.ya, self.px - vx, self.py - vy)
        else:
            own = self.ya[self.py.astype(int), self.px.astype(int)]
            oth = bilinear(self.yb, self.px + vx, self.py + vy)
        error = np.abs(own - oth).mean(axis=(2, 3))
        if self.s > 2 and self.color_a is not None:
            # Four chroma probes disambiguate coarse luma aliases. Finest
            # search stays unchanged to preserve thin-detail precision/cost.
            own_rgb, other_rgb = ((self.color_b, self.color_a) if grid == "b"
                                  else (self.color_a, self.color_b))
            sign = -1 if grid == "b" else 1
            chroma = np.zeros(error.shape, np.float32)
            for dy, dx in ((0, 0), (0, 3), (3, 0), (3, 3)):
                x = (self.px[..., dy, dx] + 0.5) * self.s * 0.5 - 0.5
                y = (self.py[..., dy, dx] + 0.5) * self.s * 0.5 - 0.5
                ca = bilinear(own_rgb, x, y)
                cb = bilinear(other_rgb, x + sign * v[..., 0] * self.s * 0.5,
                              y + sign * v[..., 1] * self.s * 0.5)
                chroma += np.abs((ca[..., 0] - ca[..., 1]) - (cb[..., 0] - cb[..., 1]))
                chroma += np.abs((ca[..., 2] - ca[..., 1]) - (cb[..., 2] - cb[..., 1]))
            error += 0.25 * np.maximum(chroma / 8 - 0.08, 0)
        return error


def best_of(level, cands, pens, grid):
    costs = np.stack([level.cost(c, grid) + p for c, p in zip(cands, pens)])
    i = np.argmin(costs, 0)
    v = np.take_along_axis(np.stack(cands), i[None, ..., None], 0)[0]
    return v, np.take_along_axis(costs, i[None], 0)[0]


def vector_median(v):
    """3x3 vector median (FSR 3's per-level filter): removes isolated outliers."""
    bh, bw, _ = v.shape
    p = np.pad(v, ((1, 1), (1, 1), (0, 0)), mode="edge")
    nb = np.stack([p[1 + dy:1 + dy + bh, 1 + dx:1 + dx + bw] for dy in (-1, 0, 1) for dx in (-1, 0, 1)], 0)
    d = np.abs(nb[:, None] - nb[None, :]).sum(-1).sum(1)
    return np.take_along_axis(nb, np.argmin(d, 0)[None, ..., None], 0)[0]


def upsample_blocks(v, bh, bw):
    """Parent vectors (x2 in level pixels) for each child block, plus 4 neighbours."""
    ph, pw = v.shape[:2]
    iy = np.clip(np.arange(bh) // 2, 0, ph - 1)
    ix = np.clip(np.arange(bw) // 2, 0, pw - 1)
    out = []
    for dy, dx in ((0, 0), (-1, 0), (1, 0), (0, -1), (0, 1)):
        out.append(2.0 * v[np.clip(iy + dy, 0, ph - 1)][:, np.clip(ix + dx, 0, pw - 1)])
    return out


def resample_field(v, bh, bw, factor):
    """Block field resampled to another grid (nearest), vectors scaled by factor."""
    iy = np.clip((np.arange(bh) * v.shape[0]) // bh, 0, v.shape[0] - 1)
    ix = np.clip((np.arange(bw) * v.shape[1]) // bw, 0, v.shape[1] - 1)
    return v[iy][:, ix] * factor


def camera_field(H, level, grid):
    """Camera motion per block in level pixels (H in full-res pixels, A -> B)."""
    s = level.s
    cx, cy = (level.cx + 0.5) * s - 0.5, (level.cy + 0.5) * s - 0.5
    if grid == "a":
        bx, by = apply_h(H, cx, cy)
        return np.stack([(bx - cx) / s, (by - cy) / s], -1).astype(np.float32)
    Hi = np.linalg.inv(H)
    ax, ay = apply_h(Hi, cx, cy)
    return np.stack([(cx - ax) / s, (cy - ay) / s], -1).astype(np.float32)


def estimate(pyr_a, pyr_b, grid, H, temporal, color_a=None, color_b=None):
    """Block vectors at 1/2 resolution (full-res pixels), coarse to fine."""
    v = None
    for l in range(LEVELS, 0, -1):
        lv = Level(pyr_a[l], pyr_b[l], 2 ** l, color_a, color_b)
        cands, pens = [], []
        if v is None:
            r = COARSE_R
            for dy in range(-r, r + 1):
                for dx in range(-r, r + 1):
                    cands.append(np.full((lv.bh, lv.bw, 2), (dx, dy), np.float32))
                    pens.append(PEN_UPDATE * 0.25 * (abs(dx) + abs(dy)))
        else:
            for c in upsample_blocks(v, lv.bh, lv.bw):
                cands.append(c)
                pens.append(0.0)
        if temporal is not None:
            cands.append(resample_field(temporal, lv.bh, lv.bw, 1.0 / lv.s))
            pens.append(0.0)
        for Hk in ([] if H is None else (H if isinstance(H, list) else [H])):
            cands.append(camera_field(Hk, lv, grid))
            pens.append(0.0)
        cands.append(np.zeros((lv.bh, lv.bw, 2), np.float32))
        pens.append(PEN_UPDATE)
        v, c = best_of(lv, cands, pens, grid)
        steps = (1.0, 0.5) if l == 1 else (1.0,)
        for st in steps:
            cands, pens = [v], [0.0]
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx or dy:
                        cands.append(v + np.array([dx, dy], np.float32) * st)
                        pens.append(PEN_UPDATE * st)
            v, c = best_of(lv, cands, pens, grid)
        # 3DRS-style spatial propagation: each block tries its neighbours'
        # vectors from this level; a smoothness term against the 3x3 median
        # lets the true motion spread over periodic textures (tiles, grass)
        for _ in range(PROPAGATE):
            med = vector_median(v)
            cands = [v, med]
            p = np.pad(v, ((1, 1), (1, 1), (0, 0)), mode="edge")
            for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                cands.append(p[1 + dy:1 + dy + lv.bh, 1 + dx:1 + dx + lv.bw])
            costs = np.stack([lv.cost(cnd, grid) + SMOOTH * np.abs(cnd - med).sum(-1) for cnd in cands])
            i = np.argmin(costs, 0)
            v = np.take_along_axis(np.stack(cands), i[None, ..., None], 0)[0]
        v = vector_median(v)
        c = lv.cost(v, grid)
    return v * 2.0, c  # level 1 -> full-res pixels; 8x8 full-res blocks


# ----------------------------------------------------------------- per-pixel fields

def patch_cost(own, other):
    d = np.abs(own - other).sum(-1)
    return uniform_filter(d, 3, mode="nearest")


def pixel_field(a, b, blocks, H, grid, hud, extra=None):
    """Per-pixel vectors on frame A's or B's grid: best of 3x3 blocks + camera."""
    h, w = a.shape[:2]
    bh, bw = blocks.shape[:2]
    py, px = np.meshgrid(np.arange(h, dtype=np.float32), np.arange(w, dtype=np.float32), indexing="ij")
    bi = np.clip((py // 8).astype(int), 0, bh - 1)
    bj = np.clip((px // 8).astype(int), 0, bw - 1)
    own = b if grid == "b" else a
    other = a if grid == "b" else b
    sign = -1.0 if grid == "b" else 1.0
    cands = []
    for dy in (0, -1, 1):
        for dx in (0, -1, 1):
            cands.append(blocks[np.clip(bi + dy, 0, bh - 1), np.clip(bj + dx, 0, bw - 1)])
    if extra is not None:
        ev, eok = extra
        for dy in (0, -1, 1):
            for dx in (0, -1, 1):
                yy, xx = np.clip(bi + dy, 0, bh - 1), np.clip(bj + dx, 0, bw - 1)
                cands.append(np.where(eok[yy, xx][..., None], ev[yy, xx], blocks[bi, bj]))
    if H is not None:
        if grid == "a":
            qx, qy = apply_h(H, px, py)
            cands.append(np.stack([qx - px, qy - py], -1).astype(np.float32))
        else:
            qx, qy = apply_h(np.linalg.inv(H), px, py)
            cands.append(np.stack([px - qx, py - qy], -1).astype(np.float32))
    cands.append(np.zeros((h, w, 2), np.float32))  # still on screen (carried weapon, character)
    best_c = np.full((h, w), np.inf, np.float32)
    best_v = np.zeros((h, w, 2), np.float32)
    for k, v in enumerate(cands):
        ox, oy = px + sign * v[..., 0], py + sign * v[..., 1]
        c = patch_cost(own, bilinear(other, ox, oy)) + (0.0 if k == 0 else PIX_MARGIN)
        c = np.where(inside(ox, oy, w, h), c, np.inf)
        better = c < best_c
        best_c = np.where(better, c, best_c)
        best_v = np.where(better[..., None], v, best_v)
    best_v[hud] = 0.0
    return best_v, best_c


# ----------------------------------------------------------------- orphan objects

def masked_down(y, m, f):
    """Downsample by f keeping thin structures: masked mean of y, max of m."""
    h, w = y.shape[0] // f * f, y.shape[1] // f * f
    yy = (y * m)[:h, :w].reshape(h // f, f, w // f, f).sum(axis=(1, 3))
    mm = m[:h, :w].reshape(h // f, f, w // f, f)
    cnt = mm.sum(axis=(1, 3))
    return yy / np.maximum(cnt, 1e-6), mm.max(axis=(1, 3))


def orphan_search(y_own, m_own, y_oth, m_oth, sign, blocks):
    """Wide search for blocks the camera and block field leave unexplained.

    Only residual pixels take part on both sides (an object is matched to the
    object, not to the background around it), and the 1/4-resolution images
    keep a thin object alive by max-pooling the residual mask.  Returns
    full-resolution vectors (A -> B) per 8x8 block and a validity mask.
    """
    bh, bw = blocks.shape[:2]
    f = 4
    yo, mo = masked_down(y_own, m_own, f)
    yt, mt = masked_down(y_oth, m_oth, f)
    # orphan blocks: 8x8 full-res blocks with residual pixels
    share = m_own[: bh * 8, : bw * 8].reshape(bh, 8, bw, 8).mean(axis=(1, 3))
    cand = share > ORPHAN_SHARE
    # at most ORPHAN_CAP blocks, strongest (most residual pixels) first; ties at
    # the limit are all left out (GPU: lmc_orphan pass 1)
    cnt = np.rint(share * 64).astype(int)
    hist = np.bincount(cnt[cand], minlength=65)
    acc, thr = 0, 64
    for c in range(64, int(np.floor(ORPHAN_SHARE * 64)), -1):
        if acc + hist[c] > ORPHAN_CAP:
            break
        acc += hist[c]
        thr = c - 1
    oi, oj = np.nonzero(cand & (cnt > thr))
    out = np.zeros((bh, bw, 2), np.float32)
    ok = np.zeros((bh, bw), bool)
    if oi.size == 0:
        return out, ok
    # each orphan block = 2x2 cells at 1/4 res
    cy = (oi[:, None] * 2 + np.array([0, 0, 1, 1])[None]).astype(np.float32)
    cx = (oj[:, None] * 2 + np.array([0, 1, 0, 1])[None]).astype(np.float32)
    own = yo[np.minimum(cy.astype(int), yo.shape[0] - 1), np.minimum(cx.astype(int), yo.shape[1] - 1)]
    wown = mo[np.minimum(cy.astype(int), mo.shape[0] - 1), np.minimum(cx.astype(int), mo.shape[1] - 1)]
    r = ORPHAN_R // f
    best = np.full(oi.size, np.inf, np.float32)
    bv = np.zeros((oi.size, 2), np.float32)
    H, W = yt.shape
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            ty, tx = cy + sign * dy, cx + sign * dx
            iny = (ty >= 0) & (ty < H) & (tx >= 0) & (tx < W)
            tyi, txi = np.clip(ty, 0, H - 1).astype(int), np.clip(tx, 0, W - 1).astype(int)
            wt = mt[tyi, txi] * iny
            wgt = wown * wt
            c = (np.abs(own - yt[tyi, txi]) * wgt).sum(1) / np.maximum(wgt.sum(1), 1e-6)
            # target must be residual too; prefer covering more of the block
            c = np.where(wgt.sum(1) >= 0.5 * np.maximum(wown.sum(1), 1e-6), c, np.inf)
            c = c + 0.0005 * (abs(dx) + abs(dy))
            better = c < best
            best = np.where(better, c, best)
            bv[better] = (dx * f, dy * f)
    # refine at full resolution, +-3 px, on the residual pixels of each block
    py0, px0 = oi * 8, oj * 8
    oy, ox = np.meshgrid(np.arange(8), np.arange(8), indexing="ij")
    ys = (py0[:, None, None] + oy).astype(np.float32)
    xs = (px0[:, None, None] + ox).astype(np.float32)
    Hf, Wf = y_oth.shape
    yown = y_own[np.minimum(ys.astype(int), Hf - 1), np.minimum(xs.astype(int), Wf - 1)]
    wmask = m_own[np.minimum(ys.astype(int), Hf - 1), np.minimum(xs.astype(int), Wf - 1)]
    fb = np.full(oi.size, np.inf, np.float32)
    fv = bv.copy()
    for ddy in range(-3, 4):
        for ddx in range(-3, 4):
            v = bv + np.array([ddx, ddy], np.float32)
            tx = xs + sign * v[:, 0, None, None]
            ty = ys + sign * v[:, 1, None, None]
            c = (np.abs(yown - bilinear(y_oth, tx, ty)) * wmask).sum((1, 2)) / np.maximum(wmask.sum((1, 2)), 1)
            better = c < fb
            fb = np.where(better, c, fb)
            fv[better] = v[better]
    good = np.isfinite(best) & (fb < ORPHAN_MAX_COST)
    out[oi[good], oj[good]] = fv[good]
    ok[oi[good], oj[good]] = True
    return out, ok


# ----------------------------------------------------------------- generator

@register("lmc")
class LMC:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.prev = None
        self.still = np.zeros((h, w), np.int32)
        self.temporal = None   # B-grid block field of the previous pair (A -> B motion)
        self.H = None
        self.debug = {}

    def push(self, frame, ts):
        b = frame.astype(np.float32) / 255.0
        yb = luma(b)
        pyr_b = [yb]
        for _ in range(LEVELS):
            pyr_b.append(down(pyr_b[-1]))
        prev, self.prev = self.prev, (b, pyr_b)
        if prev is None:
            return []
        a, pyr_a = prev
        # 1. HUD: still for HUD_PAIRS pairs (counted only while the scene moves)
        still_now = np.abs(b - a).max(-1) < STILL
        moving_scene = self.H is not None and np.hypot(*(np.array(apply_h(self.H, self.w / 2, self.h / 2))
                                                         - np.array([self.w / 2, self.h / 2]))) > 1.0
        self.still = np.where(still_now, self.still + (1 if moving_scene else 0), 0)
        hud = self.still >= HUD_PAIRS
        # 2-3. block vectors on B's grid, camera fit, refine with the camera candidate
        temporal = self.temporal
        vb, cb = estimate(pyr_a, pyr_b, "b", self.H, temporal)
        H, inl = self._fit_camera(vb, cb, hud, yb)
        vb, cb = estimate(pyr_a, pyr_b, "b", H, temporal)
        va, ca = estimate(pyr_a, pyr_b, "a", H, vb)
        self.temporal = vb
        self.H = H
        self.debug = {"inliers": inl, "hud": float(hud.mean())}
        # 4. per-pixel fields on both grids
        pvb, pcb = pixel_field(a, b, vb, H, "b", hud)
        pva, pca = pixel_field(a, b, va, H, "a", hud)
        self.orphans = None
        if ORPHANS:
            rb = ((pcb > RESID) & ~hud).astype(np.float32)
            ra = ((pca > RESID) & ~hud).astype(np.float32)
            ob = orphan_search(pyr_b[0], rb, pyr_a[0], ra, -1.0, vb)
            oa = orphan_search(pyr_a[0], ra, pyr_b[0], rb, 1.0, va)
            self.orphans = (ob, oa)
            if ob[1].any():
                pvb, pcb = pixel_field(a, b, vb, H, "b", hud, ob)
            if oa[1].any():
                pva, pca = pixel_field(a, b, va, H, "a", hud, oa)
            self.debug["orphans"] = (int(ob[1].sum()), int(oa[1].sum()))
        return [self._synth(a, b, vb, pva, pvb, H, hud, t) for t in ts]

    def _fit_camera(self, vb, cb, hud, yb):
        bh, bw = vb.shape[:2]
        cy, cx = np.meshgrid((np.arange(bh) + 0.5) * 8 - 0.5, (np.arange(bw) + 0.5) * 8 - 0.5, indexing="ij")
        # texture weight: blocks with detail, not HUD, decent match
        g = np.abs(np.diff(yb, axis=1, prepend=yb[:, :1])) + np.abs(np.diff(yb, axis=0, prepend=yb[:1]))
        tex = g[: bh * 8, : bw * 8].reshape(bh, 8, bw, 8).mean(axis=(1, 3))
        hudb = hud[: bh * 8, : bw * 8].reshape(bh, 8, bw, 8).mean(axis=(1, 3)) > 0.2
        w0 = ((tex > 0.01) & ~hudb & (cb < 0.05)).astype(np.float64)
        bx, by = (cx + 0.5) - 0.5, cy
        # B-grid vectors: point in A = x - v; fit A -> B
        H, inl = robust_camera((bx - vb[..., 0]).ravel(), (by - vb[..., 1]).ravel(),
                               vb[..., 0].ravel(), vb[..., 1].ravel(), w0.ravel())
        return (H if inl > 0.3 else None), inl

    def _synth(self, a, b, vb, pva, pvb, H, hud, t):
        h, w = self.h, self.w
        py, px = np.meshgrid(np.arange(h, dtype=np.float32), np.arange(w, dtype=np.float32), indexing="ij")
        bh, bw = vb.shape[:2]
        bi = np.clip((py // 8).astype(int), 0, bh - 1)
        bj = np.clip((px // 8).astype(int), 0, bw - 1)
        cands = [pvb, pva]
        if self.orphans is not None:
            for ev, eok in self.orphans:
                if eok.any():
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            yy, xx = np.clip(bi + dy, 0, bh - 1), np.clip(bj + dx, 0, bw - 1)
                            cands.append(np.where(eok[yy, xx][..., None], ev[yy, xx], pvb))
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                cands.append(vb[np.clip(bi + dy, 0, bh - 1), np.clip(bj + dx, 0, bw - 1)])
        if H is not None:
            qx, qy = apply_h(np.linalg.inv(H), px, py)
            cands.append(np.stack([px - qx, py - qy], -1).astype(np.float32))
        cands.append(np.zeros((h, w, 2), np.float32))
        best_s = np.full((h, w), -np.inf, np.float32)
        out = a * (1 - t) + b * t
        offs = [(dy, dx) for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
        for k, v in enumerate(cands):
            # evaluate only where this candidate differs from the per-pixel
            # fields (evaluated everywhere first): same result, far cheaper
            if k < 2:
                sel = np.ones((h, w), bool)
            else:
                sel = (np.abs(v - pvb).max(-1) > 0.25) & (np.abs(v - pva).max(-1) > 0.25)
            ys, xs = np.nonzero(sel)
            if ys.size == 0:
                continue
            vv = v[ys, xs]
            fx, fy = xs.astype(np.float32), ys.astype(np.float32)
            ax, ay = fx - t * vv[:, 0], fy - t * vv[:, 1]
            bx, by = fx + (1 - t) * vv[:, 0], fy + (1 - t) * vv[:, 1]
            ia, ib = inside(ax, ay, w, h), inside(bx, by, w, h)
            tol = 1.0 + 0.1 * np.hypot(vv[:, 0], vv[:, 1])
            # validity: the frame's own pixel at the trajectory end moves with v
            ok_a = ia & (np.hypot(*(bilinear(pva, ax, ay) - vv).T) < tol)
            ok_b = ib & (np.hypot(*(bilinear(pvb, bx, by) - vv).T) < tol)
            ca, cbb = bilinear(a, ax, ay), bilinear(b, bx, by)
            err = np.zeros(ys.size, np.float32)
            for dy, dx in offs:  # 3x3 two-sided match along the trajectory
                if dy == 0 and dx == 0:
                    err += np.abs(ca - cbb).sum(-1)
                else:
                    err += np.abs(nearest(a, ax + dx, ay + dy) - nearest(b, bx + dx, by + dy)).sum(-1)
            err /= 81.0
            both = ok_a & ok_b
            pen = 0.0 if k < 2 else 0.01
            score = np.where(both, 2.0 - err, np.where(ok_a | ok_b, 1.0, -err)) - pen
            col = np.where(both[:, None], ca * (1 - t) + cbb * t,
                           np.where(ok_a[:, None], ca, np.where(ok_b[:, None], cbb, ca * (1 - t) + cbb * t)))
            better = score > best_s[ys, xs]
            best_s[ys[better], xs[better]] = score[better]
            out[ys[better], xs[better]] = col[better]
        # no trajectory valid on either side: the motion was not found (thin,
        # fast or rotating object).  A co-located blend reads as motion blur
        # and is stable from frame to frame; a guessed vector would boil.
        if ORPHAN_BLEND:
            orphan = best_s < 0.5
            out = np.where(orphan[..., None], a * (1 - t) + b * t, out)
        out[hud] = (b if t >= 0.5 else a)[hud]
        return out

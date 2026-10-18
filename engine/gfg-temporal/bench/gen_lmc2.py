"""LMC2: the layered motion candidate in the shape it will have on the GPU.

Same layers as gen_lmc.py (HUD, camera homography, predictive block search,
orphan object search, per-pixel vectors, trajectory test), restructured so
every stage maps to a compute pass with a known cost on the Deck:

  per pair (shared by the generated frames)
    luma pyramid + HUD still counter          full res, one read of each frame
    block search, B grid and A grid           1/2 res, 8x8 full-res blocks;
                                              camera candidate = previous pair's
                                              homography (one pass, no re-run)
    camera fit                                one workgroup, IRLS on 16k vectors
    orphan search                             unexplained blocks only
    per-pixel vectors, both grids             1/2 res, 3x3 patch, block-constant
                                              candidates (LDS tiles on the GPU)
  per generated frame
    trajectory selection                      1/2 res, 1x1 colour error
    compose                                   full res: each pixel picks among
                                              the 4 nearest 1/2-res choices by
                                              its own two-sided error, then one
                                              two-sided (or one-sided) fetch
"""
from __future__ import annotations

import numpy as np
from scipy.ndimage import uniform_filter

import gen_lmc as L
from methods import register

MAX_LAYERS = 3     # motion planes: camera/far scene, ground, a wall
MIN_LAYER_SHARE = 0.10
PALETTE = 4
ORPHAN_RESID = 0.15  # half-res 3x3 cost above which a pixel is unexplained
RANSAC_ITERS = 128
MARGIN = 0.06      # half-res 3x3 RGB SAD (mean per pixel x 9) other candidates must beat
TOL_PX = 0.75      # trajectory consistency, half-res pixels (+10 % of |v|)


def half(rgb):
    h, w = rgb.shape[0] // 2 * 2, rgb.shape[1] // 2 * 2
    r = rgb[:h, :w]
    return 0.25 * (r[0::2, 0::2] + r[1::2, 0::2] + r[0::2, 1::2] + r[1::2, 1::2])


def grid_coords(h, w):
    py, px = np.meshgrid(np.arange(h, dtype=np.float32), np.arange(w, dtype=np.float32), indexing="ij")
    return py, px


def block_cands(blocks, bi, bj, extra=None, base=None):
    bh, bw = blocks.shape[:2]
    out = []
    for dy in (0, -1, 1):
        for dx in (0, -1, 1):
            out.append(blocks[np.clip(bi + dy, 0, bh - 1), np.clip(bj + dx, 0, bw - 1)])
    if extra is not None and extra[1].any():
        ev, eok = extra
        for dy in (0, -1, 1):
            for dx in (0, -1, 1):
                yy, xx = np.clip(bi + dy, 0, bh - 1), np.clip(bj + dx, 0, bw - 1)
                out.append(np.where(eok[yy, xx][..., None], ev[yy, xx], out[0]))
    return out


def camera_vec(H, px, py, grid, scale):
    """Camera motion (A -> B) in this grid's pixels at grid points (px, py)."""
    fx, fy = (px + 0.5) * scale - 0.5, (py + 0.5) * scale - 0.5
    if grid == "a":
        qx, qy = L.apply_h(H, fx, fy)
        return np.stack([(qx - fx) / scale, (qy - fy) / scale], -1).astype(np.float32)
    qx, qy = L.apply_h(np.linalg.inv(H), fx, fy)
    return np.stack([(fx - qx) / scale, (fy - qy) / scale], -1).astype(np.float32)


def half_field(ah, bh_, blocks, orph, H, grid, hudh, palette=()):
    """Per-pixel vectors at 1/2 res (half-res pixel units).

    Cost of candidate v at pixel p: sum over the 3x3 window q of
    |own(q) - other(q + sign * v)|, with p's own v for the whole window (the
    GPU evaluates each candidate once per 8x8 block over an LDS apron).
    Candidate order: own block, its 8 neighbours, the orphan vectors of those
    9 blocks, the layers, the palette, zero; others must beat the own block
    by MARGIN."""
    h, w = ah.shape[:2]
    py, px = grid_coords(h, w)
    bi = np.clip((py // 4).astype(int), 0, blocks.shape[0] - 1)
    bj = np.clip((px // 4).astype(int), 0, blocks.shape[1] - 1)
    cands = [c * 0.5 for c in block_cands(blocks, bi, bj, orph)]
    for Hk in (H or []):
        cands.append(camera_vec(Hk, px, py, grid, 2.0))
    for pv in palette:
        cands.append(np.broadcast_to(pv * 0.5, (h, w, 2)).astype(np.float32))
    cands.append(np.zeros((h, w, 2), np.float32))
    own, oth = (bh_, ah) if grid == "b" else (ah, bh_)
    sign = -1.0 if grid == "b" else 1.0
    best_c = np.full((h, w), np.inf, np.float32)
    best_v = np.zeros((h, w, 2), np.float32)
    for k, v in enumerate(cands):
        ox, oy = px + sign * v[..., 0], py + sign * v[..., 1]
        c = np.zeros((h, w), np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                qy = np.clip(py + dy, 0, h - 1).astype(int)
                qx = np.clip(px + dx, 0, w - 1).astype(int)
                c += np.abs(own[qy, qx] - L.bilinear(oth, ox + dx, oy + dy)).sum(-1)
        c = c / 9.0 + (0.0 if k == 0 else MARGIN)
        c = np.where(L.inside(ox, oy, w, h), c, np.inf)
        better = c < best_c
        best_c = np.where(better, c, best_c)
        best_v = np.where(better[..., None], v, best_v)
    best_v[hudh] = 0.0
    return best_v, best_c


def pcg(state):
    """PCG32 step (same constants as the GLSL hash): returns (new state, value)."""
    state = (state * 747796405 + 2891336453) & 0xFFFFFFFF
    word = (((state >> ((state >> 28) + 4)) ^ state) * 277803737) & 0xFFFFFFFF
    return state, ((word >> 22) ^ word) & 0xFFFFFFFF


def fit_h_ls(src, dst, w):
    """Homography with h33 = 1 by weighted normal equations on Hartley-normalised
    points: an 8x8 solve the GPU does in one workgroup (float32)."""
    def norm(p, wt):
        c = (p * wt[:, None]).sum(0) / max(wt.sum(), 1e-9)
        d = (np.linalg.norm(p - c, axis=1) * wt).sum() / max(wt.sum(), 1e-9)
        sc = np.sqrt(2) / max(d, 1e-6)
        return (p - c) * sc, np.array([[sc, 0, -sc * c[0]], [0, sc, -sc * c[1]], [0, 0, 1]])
    ps, Ts = norm(src, w)
    pd, Td = norm(dst, w)
    x, y, u, v = ps[:, 0], ps[:, 1], pd[:, 0], pd[:, 1]
    z, o = np.zeros_like(x), np.ones_like(x)
    A = np.concatenate([np.stack([x, y, o, z, z, z, -u * x, -u * y], 1),
                        np.stack([z, z, z, x, y, o, -v * x, -v * y], 1)])
    rhs = np.concatenate([u, v])
    ww = np.concatenate([w, w])
    M = (A * ww[:, None]).T @ A + 1e-9 * np.eye(8)
    h = np.linalg.solve(M, (A * ww[:, None]).T @ rhs)
    H = np.append(h, 1.0).reshape(3, 3)
    H = np.linalg.inv(Td) @ H @ Ts
    return H / H[2, 2]


def irls_h(xs, ys, vx, vy, w0, iters=6):
    """Tukey IRLS with fit_h_ls (GPU form of gen_lmc.robust_camera)."""
    src = np.stack([xs, ys], 1)
    dst = src + np.stack([vx, vy], 1)
    w = w0.astype(np.float64).copy()
    if (w > 0).sum() < 8:
        return None
    H = None
    for _ in range(iters):
        H = fit_h_ls(src, dst, w)
        px, py = L.apply_h(H, xs, ys)
        r = np.hypot(px - dst[:, 0], py - dst[:, 1])
        c = max(2.0, 2.5 * hist_median(r[w0 > 0]))
        w = w0 * np.where(r < c, (1 - (r / c) ** 2) ** 2, 0.0)
    return H


def hist_median(r):
    """Median by a 256-bin histogram of 0.25 px (GPU form: one LDS histogram);
    returns the centre of the bin holding the middle element."""
    if r.size == 0:
        return 0.0
    b = np.clip((r / 0.25).astype(np.int64), 0, 255)
    cum = np.cumsum(np.bincount(b, minlength=256))
    k = int(np.searchsorted(cum, (r.size + 1) // 2))
    return (k + 0.5) * 0.25


def motion_palette(vb, w0, k=PALETTE):
    """The k most frequent block vectors.  Background motion is always among
    them, so a pixel next to an object still has the right candidate when all
    of its neighbouring blocks took the object's motion (the halo around a
    still object on a moving scene).

    GPU form: a 64x64 histogram of 8 px bins (LDS), the fullest bin, then a
    17x17 histogram of 0.5 px steps inside it; found blocks are removed and the
    next mode is searched.  Modes under 1 % of the blocks are ignored."""
    v = vb.reshape(-1, 2)
    live = w0.ravel() > 0
    n = max(int(live.sum()), 1)
    out = []
    for _ in range(k):
        q = np.clip(np.floor((v + 256.0) / 8.0).astype(int), 0, 63)
        hist = np.zeros(64 * 64, np.int64)
        np.add.at(hist, q[live, 1] * 64 + q[live, 0], 1)
        b = int(np.argmax(hist))
        if hist[b] < 0.01 * n:
            break
        bx, by = b % 64, b // 64
        inb = live & (q[:, 0] == bx) & (q[:, 1] == by)
        f = np.clip(np.floor((v - (np.array([bx, by]) * 8.0 - 256.0)) / 0.5 + 0.5).astype(int), 0, 16)
        fh = np.zeros(17 * 17, np.int64)
        np.add.at(fh, f[inb, 1] * 17 + f[inb, 0], 1)
        fb = int(np.argmax(fh))
        mode = np.array([bx * 8.0 - 256.0 + (fb % 17) * 0.5, by * 8.0 - 256.0 + (fb // 17) * 0.5], np.float32)
        out.append(mode)
        live = live & (np.abs(v - mode).max(-1) > 1.5)
    return out


def ransac_plane(xs, ys, vx, vy, w0, seed, iters=RANSAC_ITERS, seeds=()):
    """Best homography by inlier count from 4-block samples (PCG32 sampling,
    hypotheses spread at least 120 px apart), then IRLS on its inliers.
    Multi-plane scenes need this: an IRLS fit over all blocks settles on a
    compromise that fits no plane."""
    idx = np.nonzero(w0 > 0)[0]
    if idx.size < 12:
        return None, None
    dx, dy = xs + vx, ys + vy
    tol = 1.5 + 0.03 * np.hypot(vx, vy)
    hyps = list(seeds)
    for it in range(iters):
        st = (seed * 9781 + it * 6271 + 1) & 0xFFFFFFFF
        pick = []
        for _ in range(4):
            st, r = pcg(st)
            pick.append(idx[r % idx.size])
        p = np.stack([xs[pick], ys[pick]], 1)
        if np.min([np.hypot(*(p[i] - p[j])) for i in range(4) for j in range(i + 1, 4)]) < 120:
            continue  # clustered samples give a homography that fits nothing else
        try:
            hyps.append(fit_h_ls(p, np.stack([dx[pick], dy[pick]], 1), np.ones(4)))
        except np.linalg.LinAlgError:
            continue
    best_n, best_in = 0, None
    for H in hyps:
        if not np.all(np.isfinite(H)):
            continue
        px, py = L.apply_h(H, xs[idx], ys[idx])
        inl = np.hypot(px - dx[idx], py - dy[idx]) < tol[idx]
        n = int(inl.sum())
        if n > best_n:
            best_n, best_in = n, idx[inl]
    if best_in is None or best_in.size < 8:
        return None, None
    wref = np.zeros_like(w0)
    wref[best_in] = 1.0
    H = irls_h(xs, ys, vx, vy, wref)
    if H is None or not np.all(np.isfinite(H)):
        return None, None
    px, py = L.apply_h(H, xs, ys)
    explained = (np.hypot(px - dx, py - dy) < tol) & (w0 > 0)
    return H, explained


def fit_layers(vb, cb, hud, yb, seed, prev=()):
    """Sequential planes: the largest first (camera rotation / far scene or the
    ground), then the next largest among the remaining blocks.  A homography
    is exact for a planar surface whatever the camera does."""
    bh, bw = vb.shape[:2]
    cy, cx = np.meshgrid((np.arange(bh) + 0.5) * 8 - 0.5, (np.arange(bw) + 0.5) * 8 - 0.5, indexing="ij")
    g = np.abs(np.diff(yb, axis=1, prepend=yb[:, :1])) + np.abs(np.diff(yb, axis=0, prepend=yb[:1]))
    tex = g[: bh * 8, : bw * 8].reshape(bh, 8, bw, 8).mean(axis=(1, 3))
    hudb = hud[: bh * 8, : bw * 8].reshape(bh, 8, bw, 8).mean(axis=(1, 3)) > 0.2
    w0 = ((tex > 0.01) & ~hudb & (cb < 0.05)).astype(np.float64).ravel()
    xs, ys = (cx - vb[..., 0]).ravel(), (cy - vb[..., 1]).ravel()
    vx, vy = vb[..., 0].ravel(), vb[..., 1].ravel()
    total = max(w0.sum(), 1.0)
    layers, shares = [], []
    for _ in range(MAX_LAYERS):
        H, explained = ransac_plane(xs, ys, vx, vy, w0, seed * 4 + len(layers), seeds=prev)
        if H is None:
            break
        share = float(explained.sum() / total)
        if share < MIN_LAYER_SHARE:
            break
        layers.append(H)
        shares.append(round(share, 3))
        w0 = w0 * ~explained
    return layers, shares


# Engine variants, switched by the environment so bench.py can score them:
#   LMC_FIT_SUB=2  the planes fitted on every 2nd block in each direction (the engine's CPU fit)
#   LMC_SINGLE=1   the A grid carried from the B grid (engine cost tiers 1 and 2)
#   LMC_LAG=1      the previous pair's model (a CPU fit that overlaps the next pair; measured
#                  worse on the specular scene, so the engine waits for the fit of the pair instead)
import os as _os
LAG = _os.environ.get("LMC_LAG", "0") == "1"
FIT_SUB = int(_os.environ.get("LMC_FIT_SUB", "1"))
SINGLE = _os.environ.get("LMC_SINGLE", "0") == "1"


def estimate_a_from_b(pyr_a, pyr_b, vb, H):
    """A-grid block vectors without a second pyramid search: every B block's
    vector is carried to where its content was in A (forward splat, lowest
    cost wins), holes take their neighbours' median, then one level-1 pass
    tries the splat, the camera planes and zero and refines by +-1 and +-0.5."""
    lv = L.Level(pyr_a[1], pyr_b[1], 2)
    bh, bw = lv.bh, lv.bw
    v1 = vb / 2.0                                  # full-res px -> level-1 px
    cb = lv.cost(v1, "b")
    cy, cx = np.meshgrid(np.arange(bh), np.arange(bw), indexing="ij")
    ty = np.clip(np.round(cy - v1[..., 1] / L.BLK).astype(int), 0, bh - 1)
    tx = np.clip(np.round(cx - v1[..., 0] / L.BLK).astype(int), 0, bw - 1)
    splat = np.full((bh, bw, 2), np.nan, np.float32)
    best = np.full((bh, bw), np.inf, np.float32)
    order = np.argsort(-cb.ravel())                # the cheapest write last, so it wins
    for k in order:
        i, j = divmod(int(k), bw)
        splat[ty[i, j], tx[i, j]] = v1[i, j]
        best[ty[i, j], tx[i, j]] = cb[i, j]
    hole = np.isnan(splat[..., 0])
    filled = np.where(hole[..., None], 0.0, splat)
    med = L.vector_median(np.where(hole[..., None], v1, splat))
    filled = np.where(hole[..., None], med, filled)
    cands, pens = [filled], [0.0]
    for Hk in ([] if H is None else (H if isinstance(H, list) else [H])):
        cands.append(L.camera_field(Hk, lv, "a"))
        pens.append(0.0)
    cands.append(np.zeros((bh, bw, 2), np.float32))
    pens.append(L.PEN_UPDATE)
    v, c = L.best_of(lv, cands, pens, "a")
    for st in (1.0, 0.5):
        cands, pens = [v], [0.0]
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx or dy:
                    cands.append(v + np.array([dx, dy], np.float32) * st)
                    pens.append(L.PEN_UPDATE * st)
        v, c = L.best_of(lv, cands, pens, "a")
    v = L.vector_median(v)
    c = lv.cost(v, "a")
    return v * 2.0, c


def fit_layers_sub(vb, cb, hud, yb, seed, prev, sub):
    """fit_layers on a block grid thinned by sub in each direction (the GPU field
    still evaluates every pixel against the fitted planes)."""
    bh, bw = vb.shape[:2]
    keep = np.zeros((bh, bw), bool)
    keep[::sub, ::sub] = True
    cb2 = np.where(keep, cb, 1e9)   # thinned-out blocks fail the cb < 0.05 usability test
    return fit_layers(vb, cb2, hud, yb, seed, prev)


@register("lmc2")
class LMC2:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.prev = None
        self.still = np.zeros((h, w), np.int32)
        self.temporal = None
        self.H = None
        self.layers = []
        self.pairs = 0
        self.palette = []
        self.debug = {}

    def push(self, frame, ts):
        b = frame.astype(np.float32) / 255.0
        yb = L.luma(b)
        pyr_b = [yb]
        for _ in range(L.LEVELS):
            pyr_b.append(L.down(pyr_b[-1]))
        bh_ = half(b)
        prev, self.prev = self.prev, (b, pyr_b, bh_)
        if prev is None:
            return []
        a, pyr_a, ah = prev
        still_now = np.abs(b - a).max(-1) < L.STILL
        moving = self.temporal is not None and float(np.median(np.abs(self.temporal).max(-1))) > 1.0
        self.still = np.where(still_now, self.still + (1 if moving else 0), 0)
        hud = self.still >= L.HUD_PAIRS
        hudh = half(hud.astype(np.float32)) > 0.5
        # one search per grid; the camera candidate is the previous pair's fit
        vb, cb = L.estimate(pyr_a, pyr_b, "b", self.layers or None, self.temporal, ah, bh_)
        if SINGLE:
            va, ca = estimate_a_from_b(pyr_a, pyr_b, vb, self.layers or None)
        else:
            va, ca = L.estimate(pyr_a, pyr_b, "a", self.layers or None, vb, ah, bh_)
        self.pairs += 1
        sub = FIT_SUB
        if sub > 1:
            # CPU fit on every sub-th block in each direction (cells 8*sub px apart)
            layers, shares = fit_layers_sub(vb, cb, hud, yb, self.pairs, self.layers, sub)
        else:
            layers, shares = fit_layers(vb, cb, hud, yb, self.pairs, self.layers)
        bhb, bwb = vb.shape[:2]
        hudb = hud[: bhb * 8, : bwb * 8].reshape(bhb, 8, bwb, 8).mean(axis=(1, 3)) > 0.2
        pal = motion_palette(vb, (~hudb).astype(np.float32))
        prev_layers, prev_pal = self.layers, self.palette
        self.palette = pal
        self.layers = layers
        if LAG:
            # the model of the previous pair (fitted on the CPU while this pair ran on the GPU)
            layers, pal = prev_layers, prev_pal
        self.pal_used = pal   # the select pass's palette candidates are the field's
        H = layers if layers else None
        inl = shares
        self.temporal, self.H = vb, (layers[0] if layers else None)
        # orphans need per-pixel residuals: a first per-pixel pass without them
        pvb, pcb = half_field(ah, bh_, vb, None, H, "b", hudh, pal)
        pva, pca = half_field(ah, bh_, va, None, H, "a", hudh, pal)
        rb = np.repeat(np.repeat(pcb > ORPHAN_RESID, 2, 0), 2, 1)[: self.h, : self.w] & ~hud
        ra = np.repeat(np.repeat(pca > ORPHAN_RESID, 2, 0), 2, 1)[: self.h, : self.w] & ~hud
        ob = L.orphan_search(pyr_b[0], rb.astype(np.float32), pyr_a[0], ra.astype(np.float32), -1.0, vb)
        oa = L.orphan_search(pyr_a[0], ra.astype(np.float32), pyr_b[0], rb.astype(np.float32), 1.0, va)
        if ob[1].any():
            pvb, pcb = half_field(ah, bh_, vb, ob, H, "b", hudh, pal)
        if oa[1].any():
            pva, pca = half_field(ah, bh_, va, oa, H, "a", hudh, pal)
        self.debug = {"inliers": inl, "hud": float(hud.mean()), "orphans": (int(ob[1].sum()), int(oa[1].sum()))}
        return [self._synth(a, b, ah, bh_, vb, pva, pvb, (ob, oa), H, hud, t) for t in ts]

    def _select_half(self, ah, bh_, vb, pva, pvb, orphans, H, t):
        """Trajectory test at 1/2 res: vector (half-res px) and mode per cell.
        mode 3 = both sides valid, 1 = previous only (covered), 2 = newest only
        (uncovered), 0 = none (orphan blend)."""
        h, w = ah.shape[:2]
        py, px = grid_coords(h, w)
        bi = np.clip((py // 4).astype(int), 0, vb.shape[0] - 1)
        bj = np.clip((px // 4).astype(int), 0, vb.shape[1] - 1)
        cands = [pvb, pva]
        for o in orphans:
            if o[1].any():
                ev, eok = o
                for dy in (0, -1, 1):
                    for dx in (0, -1, 1):
                        yy, xx = np.clip(bi + dy, 0, vb.shape[0] - 1), np.clip(bj + dx, 0, vb.shape[1] - 1)
                        cands.append(np.where(eok[yy, xx][..., None], ev[yy, xx] * 0.5, pvb))
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                cands.append(vb[np.clip(bi + dy, 0, vb.shape[0] - 1), np.clip(bj + dx, 0, vb.shape[1] - 1)] * 0.5)
        for Hk in (H or []):
            cands.append(camera_vec(Hk, px, py, "b", 2.0))
        for pv in self.pal_used:
            cands.append(np.broadcast_to(pv * 0.5, (h, w, 2)).astype(np.float32))
        cands.append(np.zeros((h, w, 2), np.float32))
        best_s = np.full((h, w), -np.inf, np.float32)
        best_v = np.zeros((h, w, 2), np.float32)
        best_m = np.zeros((h, w), np.int8)
        for k, v in enumerate(cands):
            ax, ay = px - t * v[..., 0], py - t * v[..., 1]
            bx, by = px + (1 - t) * v[..., 0], py + (1 - t) * v[..., 1]
            tol = TOL_PX + 0.1 * np.hypot(v[..., 0], v[..., 1])
            ok_a = L.inside(ax, ay, w, h) & (np.hypot(*(L.bilinear(pva, ax, ay) - v).transpose(2, 0, 1)) < tol)
            ok_b = L.inside(bx, by, w, h) & (np.hypot(*(L.bilinear(pvb, bx, by) - v).transpose(2, 0, 1)) < tol)
            err = np.abs(L.bilinear(ah, ax, ay) - L.bilinear(bh_, bx, by)).sum(-1) / 3.0
            both = ok_a & ok_b
            pen = 0.0 if k < 2 else 0.01
            score = np.where(both, 2.0 - err, np.where(ok_a | ok_b, 1.0, -err)) - pen
            better = score > best_s
            mode = np.where(both, 3, np.where(ok_a, 1, np.where(ok_b, 2, 0))).astype(np.int8)
            best_s = np.where(better, score, best_s)
            best_v = np.where(better[..., None], v, best_v)
            best_m = np.where(better, mode, best_m)
        best_m[best_s < 0.5] = 0
        return best_v, best_m

    def _synth(self, a, b, ah, bh_, vb, pva, pvb, orphans, H, hud, t):
        h, w = self.h, self.w
        hv, hm = self._select_half(ah, bh_, vb, pva, pvb, orphans, H, t)
        hh, hw = hv.shape[:2]
        py, px = grid_coords(h, w)
        # 4 nearest half-res cells; each full-res pixel keeps the one whose
        # trajectory matches best at full resolution (crisp object edges)
        cy, cx = (py + 0.5) / 2 - 0.5, (px + 0.5) / 2 - 0.5
        y0, x0 = np.floor(cy).astype(int), np.floor(cx).astype(int)
        best_e = np.full((h, w), np.inf, np.float32)
        out = a * (1 - t) + b * t
        for oy in (0, 1):
            for ox in (0, 1):
                yy, xx = np.clip(y0 + oy, 0, hh - 1), np.clip(x0 + ox, 0, hw - 1)
                v = hv[yy, xx] * 2.0
                m = hm[yy, xx]
                ax, ay = px - t * v[..., 0], py - t * v[..., 1]
                bx, by = px + (1 - t) * v[..., 0], py + (1 - t) * v[..., 1]
                valid_a, valid_b = L.inside(ax, ay, w, h), L.inside(bx, by, w, h)
                m = np.where((m == 3) & ~valid_a, np.where(valid_b, 2, 0), m)
                m = np.where((m == 3) & ~valid_b, np.where(valid_a, 1, 0), m)
                m = np.where(((m == 1) & ~valid_a) | ((m == 2) & ~valid_b), 0, m)
                ca, cbb = L.bilinear(a, ax, ay), L.bilinear(b, bx, by)
                e = np.abs(ca - cbb).sum(-1)
                # one-sided cells are judged by mode, not by a two-sided error
                e = np.where(m == 3, e, np.where(m == 0, 9.0, 1.5))
                col = np.where((m == 3)[..., None], ca * (1 - t) + cbb * t,
                               np.where((m == 1)[..., None], ca,
                                        np.where((m == 2)[..., None], cbb, a * (1 - t) + b * t)))
                better = e < best_e
                best_e = np.where(better, e, best_e)
                out = np.where(better[..., None], col, out)
        out[hud] = (b if t >= 0.5 else a)[hud]
        return out

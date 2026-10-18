"""NumPy reference of the GFG Temporal Engine.

This is the executable specification of the algorithm.  The Vulkan compute
shaders in ``engine/gfg-temporal/shaders`` implement the same steps in the same
order; the parity tests compare both on identical inputs.

Pipeline per pair of real frames (A = older, B = newer):

1. Luma pyramid (levels 0..L, 2x2 box).  B's pyramid is kept and reused as the
   next pair's A, so every real frame is converted once.
2. Hierarchical bilateral motion estimation per tile on the t = 0.5 grid.
   One vector field serves every intermediate timestamp of the pair.
3. Tile statistics (match error E, zero-motion difference E0/M0, vector
   smoothness S) and classification into REUSE / LIGHT / FULL with
   hysteresis, scene-cut hold and residual feedback.
4. Executors per output timestamp.  Each tile is written by exactly one
   executor, so there is no separate merge pass.

Synthetic output never feeds back as a reference image: history holds only
vectors, classes and per-tile counters.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .params import CLASS_FULL, CLASS_LIGHT, CLASS_REUSE, EXPERIMENTS, Params

LUMA = np.array([0.299, 0.587, 0.114], np.float32)
F32 = np.float32

# Neighbour order used by FULL candidates and by the smoothness statistic.
NEIGHBOURS = ((0, 0), (-1, 0), (1, 0), (0, -1), (0, 1),
              (-1, -1), (-1, 1), (1, -1), (1, 1))  # (dy, dx): self, N, S, W, E, NW, NE, SW, SE


# --------------------------------------------------------------------------- sampling

def to_float(frame):
    frame = np.asarray(frame)
    if frame.dtype == np.uint8:
        return frame.astype(F32) * F32(1.0 / 255.0)
    return frame.astype(F32, copy=False)


def luma(rgb):
    return (rgb[..., 0] * LUMA[0] + rgb[..., 1] * LUMA[1] + rgb[..., 2] * LUMA[2]).astype(F32)


def downsample(img):
    h, w = img.shape[:2]
    h2, w2 = max(1, h // 2), max(1, w // 2)
    if h == 1 or w == 1:
        ys = np.minimum(np.arange(h2) * 2, h - 1)
        xs = np.minimum(np.arange(w2) * 2, w - 1)
        ys1, xs1 = np.minimum(ys + 1, h - 1), np.minimum(xs + 1, w - 1)
        return ((img[ys][:, xs] + img[ys][:, xs1] + img[ys1][:, xs] + img[ys1][:, xs1]) * F32(0.25)).astype(F32)
    a = img[0:2 * h2:2, 0:2 * w2:2]
    b = img[0:2 * h2:2, 1:2 * w2:2]
    c = img[1:2 * h2:2, 0:2 * w2:2]
    d = img[1:2 * h2:2, 1:2 * w2:2]
    return ((a + b + c + d) * F32(0.25)).astype(F32)


def pyramid(y, levels):
    out = [y.astype(F32, copy=False)]
    for _ in range(levels):
        out.append(downsample(out[-1]))
    return out


def sample(img, x, y):
    """Bilinear sample with clamp-to-edge; pixel centres at integer coordinates."""
    h, w = img.shape[:2]
    x = np.clip(np.asarray(x, F32), 0, w - 1)
    y = np.clip(np.asarray(y, F32), 0, h - 1)
    x0f, y0f = np.floor(x), np.floor(y)
    fx, fy = (x - x0f).astype(F32), (y - y0f).astype(F32)
    x0, y0 = x0f.astype(np.intp), y0f.astype(np.intp)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    flat = img.reshape(h * w, -1) if img.ndim == 3 else img.reshape(-1)
    if img.ndim == 3:
        fx, fy = fx[..., None], fy[..., None]
    t00, t01 = flat[y0 * w + x0], flat[y0 * w + x1]
    t10, t11 = flat[y1 * w + x0], flat[y1 * w + x1]
    top = t00 + (t01 - t00) * fx
    bot = t10 + (t11 - t10) * fx
    return (top + (bot - top) * fy).astype(F32)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return (t * t * (3.0 - 2.0 * t)).astype(F32)


# --------------------------------------------------------------------------- analysis

def tile_grid(width, height, tile):
    return (width + tile - 1) // tile, (height + tile - 1) // tile


def me_window(tile, level):
    span = max(1, tile >> level)
    n = min(8, max(4, span))
    step = max(1, span // n)
    offs = (np.arange(n, dtype=F32) - F32((n - 1) / 2.0)) * F32(step)
    return offs


@dataclass
class OpCount:
    """Exact operation counts of what the reference executed (not timings)."""
    tex: dict = field(default_factory=dict)      # stage -> filtered texture fetches
    buf: dict = field(default_factory=dict)      # stage -> vector buffer loads

    def add(self, stage, tex=0, buf=0):
        self.tex[stage] = self.tex.get(stage, 0) + int(tex)
        self.buf[stage] = self.buf.get(stage, 0) + int(buf)


@dataclass
class Analysis:
    vectors: np.ndarray        # (ty, tx, 2) full-res px, A->B displacement
    err: np.ndarray            # E: bilateral match error at the final vector
    diff: np.ndarray           # E0: mean |YA - YB| over the tile
    diff_max: np.ndarray       # M0: max |YA - YB| over the tile
    smooth: np.ndarray         # S: max Chebyshev distance to the 8 neighbour vectors (diagnostic)
    probe: np.ndarray          # P: mean |YA - YB| along the interpolated field on a sparse grid
    neigh_motion: np.ndarray   # max |v|_inf over the 3x3 tile neighbourhood
    scene_cut: bool
    confidence: float


class MotionEstimator:
    def __init__(self, p: Params):
        self.p = p

    def _cost(self, pa, pb, cx, cy, offs, v, bias=True):
        """Mean |A(c+o-v/2) - B(c+o+v/2)| per tile; v shape (ty, tx, 2) in level px."""
        ox = offs[None, None, None, :]
        oy = offs[None, None, :, None]
        hx = (v[..., 0] * F32(0.5))[..., None, None]
        hy = (v[..., 1] * F32(0.5))[..., None, None]
        xs = cx[..., None, None] + ox
        ys = cy[..., None, None] + oy
        a = sample(pa, xs - hx, ys - hy)
        b = sample(pb, xs + hx, ys + hy)
        c = np.abs(a - b).mean(axis=(-2, -1), dtype=F32).astype(F32)
        if bias:
            c = c + F32(self.p.me_bias) * (np.abs(v[..., 0]) + np.abs(v[..., 1]))
        return c.astype(F32)

    def _best(self, pa, pb, cx, cy, offs, cands, ops, stage):
        best_v = cands[0]
        best_c = self._cost(pa, pb, cx, cy, offs, best_v)
        for v in cands[1:]:
            c = self._cost(pa, pb, cx, cy, offs, v)
            take = c < best_c
            best_c = np.where(take, c, best_c)
            best_v = np.where(take[..., None], v, best_v)
        n = len(cands) * offs.size * offs.size * 2 * best_c.size
        ops.add(stage, tex=n)
        return best_v.astype(F32), best_c

    def estimate(self, pyr_a, pyr_b, prev_vectors, ops):
        p = self.p
        h, w = pyr_a[0].shape
        tx, ty = tile_grid(w, h, p.tile)
        ix = np.arange(tx, dtype=F32)[None, :].repeat(ty, 0)
        iy = np.arange(ty, dtype=F32)[:, None].repeat(tx, 1)
        c0x = ix * p.tile + F32((p.tile - 1) / 2.0)
        c0y = iy * p.tile + F32((p.tile - 1) / 2.0)
        zero = np.zeros((ty, tx, 2), F32)
        temporal = prev_vectors if prev_vectors is not None else zero
        v = None
        for level in range(p.levels, -1, -1):
            s = F32(2 ** level)
            cx = (c0x - F32((2 ** level - 1) / 2.0)) / s
            cy = (c0y - F32((2 ** level - 1) / 2.0)) / s
            offs = me_window(p.tile, level)
            pa, pb = pyr_a[level], pyr_b[level]
            tp = (temporal / s).astype(F32)
            if v is None:
                cands = [tp]
                r = p.coarse_range
                for dy in range(-r, r + 1):
                    for dx in range(-r, r + 1):
                        cands.append(np.broadcast_to(np.array([dx, dy], F32), (ty, tx, 2)))
            else:
                up = v * F32(2.0)
                cands = [shift(up, dy, dx) for dy, dx in NEIGHBOURS[:5]] + [zero, tp]
            v, _ = self._best(pa, pb, cx, cy, offs, cands, ops, "analyze")
            steps = (1.0, 0.5) if level == 0 else ((1.0,) if level < p.levels else ())
            for step in steps:
                cands = [v] + [v + np.array([dx * step, dy * step], F32)
                               for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dx or dy]
                v, _ = self._best(pa, pb, cx, cy, offs, cands, ops, "analyze")
        offs0 = me_window(p.tile, 0)
        err = self._cost(pyr_a[0], pyr_b[0], c0x, c0y, offs0, v, bias=False)
        ops.add("analyze", tex=offs0.size * offs0.size * 2 * err.size)
        return v.astype(F32), err


def shift(grid, dy, dx):
    """Neighbour lookup with clamped tile indices: out[y, x] = grid[y+dy, x+dx]."""
    ty, tx = grid.shape[:2]
    ys = np.clip(np.arange(ty) + dy, 0, ty - 1)
    xs = np.clip(np.arange(tx) + dx, 0, tx - 1)
    return grid[ys][:, xs]


def tile_reduce(values, tile, how):
    h, w = values.shape
    tx, ty = tile_grid(w, h, tile)
    pad = np.full((ty * tile, tx * tile), np.nan, F32)
    pad[:h, :w] = values
    blocks = pad.reshape(ty, tile, tx, tile)
    if how == "mean":
        return np.nanmean(blocks, axis=(1, 3)).astype(F32)
    return np.nanmax(blocks, axis=(1, 3)).astype(F32)


def analyze_pair(p: Params, pyr_a, pyr_b, prev_vectors, ops):
    vectors, err = MotionEstimator(p).estimate(pyr_a, pyr_b, prev_vectors, ops)
    d = np.abs(pyr_a[0] - pyr_b[0])
    diff = tile_reduce(d, p.tile, "mean")
    diff_max = tile_reduce(d, p.tile, "max")
    ops.add("analyze", tex=2 * d.size)
    smooth = np.zeros(err.shape, F32)
    neigh = np.zeros(err.shape, F32)
    for dy, dx in NEIGHBOURS:
        n = shift(vectors, dy, dx)
        smooth = np.maximum(smooth, np.abs(n - vectors).max(axis=-1))
        neigh = np.maximum(neigh, np.abs(n).max(axis=-1))
    probe = probe_residual(pyr_a[0], pyr_b[0], vectors, p.tile, p.probe, ops)
    # "median above threshold" written as a count, so the GPU decides it with two atomics
    n = err.size
    cut = bool(2 * int((err > p.cut_thr).sum()) > n and 2 * int((diff > p.cut_thr).sum()) > n)
    confidence = float(np.clip(1.0 - probe / F32(p.cut_thr), 0.0, 1.0).mean())
    return Analysis(vectors, err, diff, diff_max, smooth, probe, neigh, cut, confidence)


def probe_offsets(tile, n):
    return ((np.arange(n, dtype=F32) + F32(0.5)) * F32(tile / n) - F32(0.5)).astype(F32)


def probe_residual(ya, yb, vectors, tile, n, ops):
    """What LIGHT would get wrong: |YA - YB| along the interpolated field at t = 0.5.

    Evaluated on an n x n grid per tile, so it costs a few fetches per tile and
    predicts the LIGHT/FULL quality gap far better than vector statistics.
    """
    ty, tx = vectors.shape[:2]
    off = probe_offsets(tile, n)
    xs = (np.arange(tx, dtype=F32)[:, None] * F32(tile) + off[None, :]).reshape(-1)
    ys = (np.arange(ty, dtype=F32)[:, None] * F32(tile) + off[None, :]).reshape(-1)
    gx, gy = np.meshgrid(xs, ys)
    v = smooth_vector(vectors, gx, gy, 0.5, tile)
    a = sample(ya, gx - F32(0.5) * v[..., 0], gy - F32(0.5) * v[..., 1])
    b = sample(yb, gx + F32(0.5) * v[..., 0], gy + F32(0.5) * v[..., 1])
    ops.add("analyze", tex=2 * gx.size, buf=8 * gx.size)
    return np.abs(a - b).reshape(ty, n, tx, n).mean(axis=(1, 3)).astype(F32)


def target_classes(p: Params, an: Analysis, experiments=frozenset()):
    reuse = (an.diff <= p.static_thr) & (an.diff_max <= p.static_max_thr) & (an.neigh_motion <= p.tile / 2)
    light = an.probe <= p.light_thr
    cls = np.full(an.err.shape, CLASS_FULL, np.int8)
    cls[light] = CLASS_LIGHT
    if "E" in experiments:
        cls[light & (np.abs(an.vectors).max(axis=-1) <= p.e_motion_thr)] = CLASS_REUSE
    cls[reuse] = CLASS_REUSE
    if not p.adaptive:
        cls[:] = p.force_class
    return cls


def apply_hysteresis(p: Params, target, state, count):
    """Upgrades apply at once; a downgrade needs `hysteresis` consecutive eligible pairs."""
    if state is None:
        return target.copy(), np.zeros(target.shape, np.int16)
    out = state.copy()
    cnt = count.copy()
    up = target > state
    down = target < state
    same = target == state
    out[up] = target[up]
    cnt[up | same] = 0
    cnt[down] += 1
    ready = down & (cnt >= p.hysteresis)
    out[ready] = target[ready]
    cnt[ready] = 0
    return out, cnt


# --------------------------------------------------------------------------- executors

def tile_pixels(tiles, tile, width, height):
    """Pixel coordinates of the listed tiles: arrays (k, T, T) and validity mask."""
    ty, tx = tiles
    r = np.arange(tile)
    xs = tx[:, None, None] * tile + r[None, None, :]
    ys = ty[:, None, None] * tile + r[None, :, None]
    xs, ys = np.broadcast_arrays(xs, ys)
    valid = (xs < width) & (ys < height)
    return xs, ys, valid


def vector_field(vectors, x, y, tile):
    u = (x - F32((tile - 1) / 2.0)) / F32(tile)
    v = (y - F32((tile - 1) / 2.0)) / F32(tile)
    return sample(vectors, u, v)


def smooth_vector(vectors, x, y, t, tile):
    v = vector_field(vectors, x, y, tile)
    k = F32(0.5 - t)
    return vector_field(vectors, x + k * v[..., 0], y + k * v[..., 1], tile)


class Frame:
    def __init__(self, rgb):
        self.rgb = to_float(rgb)
        self.h, self.w = self.rgb.shape[:2]
        self.y = luma(self.rgb)


def light_executor(fa, fb, vectors, xs, ys, t, tile, ops, single_source=False):
    xf, yf = xs.astype(F32), ys.astype(F32)
    v = smooth_vector(vectors, xf, yf, t, tile)
    ops.add("light", buf=8 * xs.size)
    if single_source:
        if t < 0.5:
            out = sample(fa.rgb, xf - F32(t) * v[..., 0], yf - F32(t) * v[..., 1])
        else:
            out = sample(fb.rgb, xf + F32(1 - t) * v[..., 0], yf + F32(1 - t) * v[..., 1])
        ops.add("light", tex=xs.size)
        return out, None
    a = sample(fa.rgb, xf - F32(t) * v[..., 0], yf - F32(t) * v[..., 1])
    b = sample(fb.rgb, xf + F32(1 - t) * v[..., 0], yf + F32(1 - t) * v[..., 1])
    ops.add("light", tex=2 * xs.size)
    out = a * F32(1 - t) + b * F32(t)
    resid = np.abs(a - b).mean(axis=-1)
    return out.astype(F32), resid.astype(F32)


def full_candidates(vectors, xs, ys, t, tile):
    ty_, tx_ = vectors.shape[:2]
    cty = np.minimum(ys // tile, ty_ - 1)
    ctx = np.minimum(xs // tile, tx_ - 1)
    vs = smooth_vector(vectors, xs.astype(F32), ys.astype(F32), t, tile)
    cands = []
    for dy, dx in NEIGHBOURS:
        cands.append(vectors[np.clip(cty + dy, 0, ty_ - 1), np.clip(ctx + dx, 0, tx_ - 1)])
    cands.append(vs)
    return np.stack(cands, 0), vs


def candidate_cost(fa, fb, x, y, c, vs, t, bias):
    """3x3 bilateral luma cost of candidate c at time t (+ bias toward the smooth field)."""
    acc = np.zeros(x.shape, F32)
    ta, tb = F32(t), F32(1 - t)
    for oy in (-1, 0, 1):
        for ox in (-1, 0, 1):
            qx, qy = x + F32(ox), y + F32(oy)
            a = sample(fa.y, qx - ta * c[..., 0], qy - ta * c[..., 1])
            b = sample(fb.y, qx + tb * c[..., 0], qy + tb * c[..., 1])
            acc += np.abs(a - b)
    acc = acc * F32(1.0 / 9.0)
    return acc + F32(bias) * (np.abs(c[..., 0] - vs[..., 0]) + np.abs(c[..., 1] - vs[..., 1]))


def choose(fa, fb, cands, vs, x, y, t, bias, ops, subset=None):
    """Index of the cheapest candidate per pixel (first minimum wins)."""
    n = cands.shape[0]
    best_i = np.zeros(x.shape, np.int8)
    best_c = None
    evaluated = 0
    for i in range(n):
        if subset is not None and not subset[i].any():
            continue
        c = candidate_cost(fa, fb, x, y, cands[i], vs, t, bias)
        mask = np.ones(x.shape, bool) if subset is None else subset[i]
        evaluated += int(mask.sum())
        if best_c is None:
            best_c = np.where(mask, c, np.inf).astype(F32)
            best_i[mask] = i
            continue
        take = mask & (c < best_c)
        best_c = np.where(take, c, best_c)
        best_i[take] = i
    ops.add("full", tex=evaluated * 18)
    return best_i


def full_executor(fa, fb, vectors, xs, ys, valid, t, p: Params, ops, experiments=frozenset(), parity=0):
    """FULL tiles: per-pixel candidate choice, occlusion-aware blend.

    xs, ys, valid have shape (k, T, T) so per-tile neighbourhoods are available
    for the checkerboard and decision-density experiments.
    """
    tile = p.tile
    cands, vs = full_candidates(vectors, xs, ys, t, tile)
    ops.add("full", buf=(8 + 9) * xs.size)
    xf, yf = xs.astype(F32), ys.astype(F32)
    density = next((e[1:] for e in ("B2x1", "B1x2", "B2x2") if e in experiments), None)
    if density is None and p.full_density == 2 and "A" not in experiments:
        density = "2x2"  # production default since te.2: one FULL decision per 2x2 block
    if density:
        bx, by = int(density[0]), int(density[2])
        lx = (xs % tile) // bx * bx
        ly = (ys % tile) // by * by
        cx = (xs - xs % tile + lx).astype(F32) + F32((bx - 1) / 2.0)
        cy = (ys - ys % tile + ly).astype(F32) + F32((by - 1) / 2.0)
        lead = ((xs % tile) % bx == 0) & ((ys % tile) % by == 0)
        lead_idx = np.nonzero(lead)
        ccands = cands[(slice(None),) + lead_idx]
        cvs = vs[lead_idx]
        sel = choose(fa, fb, ccands, cvs, cx[lead_idx], cy[lead_idx], t, p.smooth_bias, ops)
        grid = np.zeros(xs.shape, np.int8)
        grid[lead_idx] = sel
        # every pixel takes the decision of its block leader
        k, T, _ = xs.shape
        rows = (np.arange(T) // by * by)
        cols = (np.arange(T) // bx * bx)
        best = grid[:, rows][:, :, cols]
    elif "A" in experiments:
        decide = ((xs + ys + parity) % 2) == 0
        di = np.nonzero(decide)
        sel = choose(fa, fb, cands[(slice(None),) + di], vs[di], xf[di], yf[di], t, p.smooth_bias, ops)
        best = np.full(xs.shape, -1, np.int8)
        best[di] = sel
        # odd pixels: evaluate only the choices of their in-tile 4-neighbours
        subset = np.zeros((cands.shape[0],) + xs.shape, bool)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            nb = np.full(xs.shape, -1, np.int8)
            src = best
            if dy == -1:
                nb[:, 1:, :] = src[:, :-1, :]
            elif dy == 1:
                nb[:, :-1, :] = src[:, 1:, :]
            elif dx == -1:
                nb[:, :, 1:] = src[:, :, :-1]
            else:
                nb[:, :, :-1] = src[:, :, 1:]
            for i in range(cands.shape[0]):
                subset[i] |= (nb == i)
        odd = ~decide
        subset &= odd[None]
        oi = np.nonzero(odd)
        sub = subset[(slice(None),) + oi]
        sel2 = choose(fa, fb, cands[(slice(None),) + oi], vs[oi], xf[oi], yf[oi], t, p.smooth_bias, ops, subset=sub)
        best[oi] = sel2
    else:
        best = choose(fa, fb, cands, vs, xf, yf, t, p.smooth_bias, ops)
    v = np.take_along_axis(cands, best[None, ..., None].astype(np.intp), 0)[0]
    a = sample(fa.rgb, xf - F32(t) * v[..., 0], yf - F32(t) * v[..., 1])
    b = sample(fb.rgb, xf + F32(1 - t) * v[..., 0], yf + F32(1 - t) * v[..., 1])
    ops.add("full", tex=2 * xs.size)
    d = np.abs(a - b).mean(axis=-1)
    wgt = smoothstep(p.occ_lo, p.occ_hi, d)[..., None]
    blend = a * F32(1 - t) + b * F32(t)
    near = a if t < 0.5 else b
    out = blend + (near - blend) * wgt
    return out.astype(F32), d.astype(F32), v


def luma_chroma_split(fa, fb, v_pixel, xs, ys, t, ops):
    """Experiment C: luma per pixel, chroma (rgb - luma) from the 2x2 block leader."""
    xf, yf = xs.astype(F32), ys.astype(F32)
    ta, tb = F32(t), F32(1 - t)
    ya = sample(fa.y, xf - ta * v_pixel[..., 0], yf - ta * v_pixel[..., 1])
    yb = sample(fb.y, xf + tb * v_pixel[..., 0], yf + tb * v_pixel[..., 1])
    k, T, _ = xs.shape
    rows = np.arange(T) // 2 * 2
    lead_v = v_pixel[:, rows][:, :, rows]
    lx = xs[:, rows][:, :, rows].astype(F32)
    ly = ys[:, rows][:, :, rows].astype(F32)
    ca = sample(fa.rgb, lx - ta * lead_v[..., 0], ly - ta * lead_v[..., 1])
    cb = sample(fb.rgb, lx + tb * lead_v[..., 0], ly + tb * lead_v[..., 1])
    ca = ca - luma(ca)[..., None]
    cb = cb - luma(cb)[..., None]
    y = ya * F32(1 - t) + yb * F32(t)
    chroma = ca * F32(1 - t) + cb * F32(t)
    ops.add("chroma_split_bytes", tex=2 * xs.size * 2 + 2 * (xs.size // 4) * 4)
    return np.clip(chroma + y[..., None], 0.0, 1.0).astype(F32)


# --------------------------------------------------------------------------- engine

@dataclass
class PairReport:
    hold: bool
    classes: np.ndarray            # final per-tile classes after feedback (per output)
    class_share: list              # per output: fractions (reuse, light, full)
    promoted: int
    confidence: float
    ops: OpCount
    per_frame_classes: list


class TemporalEngine:
    """Stateful reference engine.  Feed real frames in order with `push`."""

    def __init__(self, params: Params | None = None, experiments=()):
        self.p = params or Params()
        unknown = set(experiments) - set(EXPERIMENTS)
        if unknown:
            raise ValueError(f"unknown experiments {sorted(unknown)}")
        self.exp = frozenset(experiments)
        self.reset()

    def reset(self):
        self.prev_frame = None
        self.prev_pyr = None
        self.prev_vectors = None
        self.cls_state = None
        self.cls_count = None

    def push(self, rgb, timestamps):
        """Add a real frame; returns synthetic frames between the previous real frame and this one."""
        frame = Frame(rgb)
        ops = OpCount()
        pyr = pyramid(frame.y, self.p.levels)
        ops.add("pyramid", tex=sum(l.size for l in pyr[1:]) * 4)
        if self.prev_frame is None:
            self.prev_frame, self.prev_pyr = frame, pyr
            return [], None
        fa, fb = self.prev_frame, frame
        outs, report = self._pair(fa, fb, self.prev_pyr, pyr, list(timestamps), ops)
        self.prev_frame, self.prev_pyr = frame, pyr
        return outs, report

    def _pair(self, fa, fb, pyr_a, pyr_b, ts, ops):
        p, exp = self.p, self.exp
        an = analyze_pair(p, pyr_a, pyr_b, self.prev_vectors, ops)
        tx, ty = tile_grid(fa.w, fa.h, p.tile)
        if an.scene_cut:
            outs = [(fa.rgb if t < 0.5 else fb.rgb).copy() for t in ts]
            ops.add("reuse", tex=len(ts) * fa.w * fa.h)
            self.prev_vectors = None
            self.cls_state = None
            self.cls_count = None
            cls = np.full((ty, tx), CLASS_REUSE, np.int8)
            return outs, PairReport(True, cls, [(1.0, 0.0, 0.0)] * len(ts), 0, an.confidence, ops, [cls] * len(ts))
        target = target_classes(p, an, exp)
        cls, cnt = apply_hysteresis(p, target, self.cls_state, self.cls_count)
        outs, shares, per_frame = [], [], []
        promoted = 0
        for k, t in enumerate(ts):
            frame_cls = cls.copy()
            if "G" in exp and k % 2 == 1:
                frame_cls[frame_cls == CLASS_FULL] = CLASS_LIGHT
            out = np.empty_like(fa.rgb)
            if "F" in exp:
                frame_cls[frame_cls == CLASS_FULL] = CLASS_LIGHT
            self._reuse(out, fa, fb, frame_cls, t, ops)
            resid = self._light(out, fa, fb, an.vectors, frame_cls, t, ops)
            if resid is not None:
                bad = (resid > p.residual_thr) & (frame_cls == CLASS_LIGHT)
                if "F" in exp:
                    frame_cls[bad] = CLASS_FULL
                    # stage 2 overwrites the LIGHT result of the flagged tiles
                elif bad.any():
                    cls[bad] = CLASS_FULL
                    cnt[bad] = 0
                    promoted += int(bad.sum())
            self._full(out, fa, fb, an.vectors, frame_cls, t, ops, parity=k)
            outs.append(out)
            n = frame_cls.size
            shares.append(tuple(float((frame_cls == c).sum()) / n for c in (CLASS_REUSE, CLASS_LIGHT, CLASS_FULL)))
            per_frame.append(frame_cls)
        self.cls_state, self.cls_count = cls, cnt
        self.prev_vectors = an.vectors
        return outs, PairReport(False, cls, shares, promoted, an.confidence, ops, per_frame)

    def _tiles(self, cls, which):
        ty, tx = np.nonzero(cls == which)
        return ty, tx

    def _scatter(self, out, xs, ys, valid, values):
        out[ys[valid], xs[valid]] = values[valid]

    def _reuse(self, out, fa, fb, cls, t, ops):
        tiles = self._tiles(cls, CLASS_REUSE)
        if not tiles[0].size:
            return
        xs, ys, valid = tile_pixels(tiles, self.p.tile, fa.w, fa.h)
        src = fa.rgb if t < 0.5 else fb.rgb
        out[ys[valid], xs[valid]] = src[ys[valid], xs[valid]]
        ops.add("reuse", tex=int(valid.sum()))

    def _light(self, out, fa, fb, vectors, cls, t, ops):
        tiles = self._tiles(cls, CLASS_LIGHT)
        resid_tiles = None
        if not tiles[0].size:
            return None
        xs, ys, valid = tile_pixels(tiles, self.p.tile, fa.w, fa.h)
        xs_c, ys_c = np.minimum(xs, fa.w - 1), np.minimum(ys, fa.h - 1)
        single = "D" in self.exp
        values, resid = light_executor(fa, fb, vectors, xs_c, ys_c, t, self.p.tile, ops, single_source=single)
        if "C" in self.exp:
            vpix = smooth_vector(vectors, xs_c.astype(F32), ys_c.astype(F32), t, self.p.tile)
            values = luma_chroma_split(fa, fb, vpix, xs_c, ys_c, t, ops)
        self._scatter(out, xs, ys, valid, values)
        resid_tiles = np.zeros(cls.shape, F32)
        if resid is not None:
            r = np.where(valid, resid, 0).sum(axis=(1, 2)) / np.maximum(valid.sum(axis=(1, 2)), 1)
            resid_tiles[tiles] = r
            return resid_tiles
        return None

    def _full(self, out, fa, fb, vectors, cls, t, ops, parity=0):
        tiles = self._tiles(cls, CLASS_FULL)
        if not tiles[0].size:
            return
        xs, ys, valid = tile_pixels(tiles, self.p.tile, fa.w, fa.h)
        xs_c, ys_c = np.minimum(xs, fa.w - 1), np.minimum(ys, fa.h - 1)
        values, _, vpix = full_executor(fa, fb, vectors, xs_c, ys_c, valid, t, self.p, ops, self.exp, parity)
        if "C" in self.exp:
            values = luma_chroma_split(fa, fb, vpix, xs_c, ys_c, t, ops)
        self._scatter(out, xs, ys, valid, values)


# --------------------------------------------------------------------------- baselines

def repeat_nearest(a, b, t):
    return to_float(a if t < 0.5 else b)


def linear_blend(a, b, t):
    return to_float(a) * F32(1 - t) + to_float(b) * F32(t)

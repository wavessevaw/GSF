"""Quality metrics for reconstructed frames against held-out ground truth."""
from __future__ import annotations

import numpy as np

from .core import luma, to_float

try:  # scikit-image is the reference SSIM implementation; keep it optional for CI
    from skimage.metrics import structural_similarity as _ssim
except Exception:  # pragma: no cover - exercised only without scikit-image
    _ssim = None


def psnr_y(out, gt):
    ey = luma(to_float(out)) - luma(to_float(gt))
    mse = float(np.mean(ey * ey))
    return 99.0 if mse <= 1e-12 else 10.0 * np.log10(1.0 / mse)


def ssim_y(out, gt):
    if _ssim is None:
        return float("nan")
    return float(_ssim(luma(to_float(out)), luma(to_float(gt)), data_range=1.0, gaussian_weights=True,
                       sigma=1.5, use_sample_covariance=False))


def temporal_error(out_prev, out, gt_prev, gt):
    """Mean |(out - out_prev) - (gt - gt_prev)| in luma: flicker and judder not present in truth."""
    d = (luma(to_float(out)) - luma(to_float(out_prev))) - (luma(to_float(gt)) - luma(to_float(gt_prev)))
    return float(np.mean(np.abs(d)))


def seam_excess(out, gt, classes, tile):
    """Extra gradient error on tile edges between different classes, relative to same-class edges.

    Positive values mean visible steps where executors meet; ~0 means no seam.
    """
    yo, yg = luma(to_float(out)), luma(to_float(gt))
    h, w = yo.shape
    errs = {True: [], False: []}
    ty, tx = classes.shape
    for i in range(1, tx):
        x = i * tile
        if x >= w:
            break
        err = np.abs(np.abs(yo[:, x] - yo[:, x - 1]) - np.abs(yg[:, x] - yg[:, x - 1]))
        rows_cls_l = classes[np.minimum(np.arange(h) // tile, ty - 1), i - 1]
        rows_cls_r = classes[np.minimum(np.arange(h) // tile, ty - 1), i]
        diff = rows_cls_l != rows_cls_r
        errs[True].append(err[diff])
        errs[False].append(err[~diff])
    for j in range(1, ty):
        y = j * tile
        if y >= h:
            break
        err = np.abs(np.abs(yo[y] - yo[y - 1]) - np.abs(yg[y] - yg[y - 1]))
        cu = classes[j - 1, np.minimum(np.arange(w) // tile, tx - 1)]
        cd = classes[j, np.minimum(np.arange(w) // tile, tx - 1)]
        diff = cu != cd
        errs[True].append(err[diff])
        errs[False].append(err[~diff])
    mixed = np.concatenate(errs[True]) if errs[True] else np.zeros(0)
    same = np.concatenate(errs[False]) if errs[False] else np.zeros(0)
    if mixed.size == 0 or same.size == 0:
        return 0.0
    return float(mixed.mean() - same.mean())

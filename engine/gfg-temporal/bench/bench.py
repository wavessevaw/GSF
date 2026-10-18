"""Temporal Engine quality bench: drop-frame protocol on rendered 90 FPS scenes.

Every scene is rendered at 90 FPS.  A generator sees every K-th frame (K = 3:
a 30 FPS game on a 90 Hz panel) and must produce the K-1 frames in between;
those are compared with the frames that were really rendered.

Scores per scene and method:
  flip       mean NVIDIA FLIP over generated frames (perceptual, lower is better)
  flip_hard  FLIP inside the "hard" mask: pixels where a plain blend is wrong,
             i.e. where motion actually has to be reconstructed
  flip_hud   FLIP inside the HUD rectangles (HUD must stay sharp and still)
  psnr       luma PSNR over the whole frame
  temporal   mean |output step - truth step| over the full output sequence
             (real + generated frames): judder and flicker
  worst      95th percentile per-frame FLIP: the bad frames a player notices

usage: python3 bench.py <frames dir> [--scenes a,b] [--methods a,b] [--pairs N]
       [--out results.json] [--dump dir]
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "reference"))
sys.path.insert(0, str(HERE))

import flip_evaluator  # noqa: E402
from PIL import Image  # noqa: E402

import methods  # noqa: E402

K = 3
WARMUP = 10
# HUD rectangles drawn by scenes/main.gd (x0, y0, x1, y1), with a small margin
HUD_RECTS = [(16, 16, 146, 146), (36, 736, 348, 766), (856, 26, 1230, 62), (1116, 736, 1270, 776),
             (624, 384, 656, 416)]


def load_scene(d: Path) -> np.ndarray:
    files = sorted(d.glob("f*.png"))[WARMUP:]
    return np.stack([np.asarray(Image.open(f).convert("RGB")) for f in files])


def to_f(img):
    return img.astype(np.float32) / 255.0 if img.dtype == np.uint8 else np.clip(img, 0, 1).astype(np.float32)


def luma(x):
    return x[..., 0] * 0.2126 + x[..., 1] * 0.7152 + x[..., 2] * 0.0722


def hud_mask(h, w):
    m = np.zeros((h, w), bool)
    for x0, y0, x1, y1 in HUD_RECTS:
        m[y0:y1, x0:x1] = True
    return m


def flip_map(ref, test):
    err, _, _ = flip_evaluator.evaluate(ref, test, "LDR", inputsRGB=True, applyMagma=False, computeMeanError=False)
    return np.asarray(err).reshape(ref.shape[:2])


def run_scene(frames: np.ndarray, method_name: str, pairs: int | None, dump: Path | None, stride: int = 1):
    n = frames.shape[0]
    reals = list(range(0, n, K))
    if pairs:
        reals = reals[: pairs + 1]
    # stride > 1: score every stride-th pair, but feed the generator the pair
    # before it too so temporal methods have one pair of history
    scored = set(range(1, len(reals), stride)) if stride > 1 else set(range(1, len(reals)))
    h, w = frames.shape[1:3]
    hud = hud_mask(h, w)
    gen = methods.make(method_name, w, h)
    ts = [k / K for k in range(1, K)]
    per_frame, seq_out, seq_gt, temps = [], [], [], []
    hard_sum = hard_n = hud_sum = hud_n = 0.0
    psnrs = []
    t0 = time.time()
    for i, r in enumerate(reals):
        if stride > 1 and i not in scored and (i + 1) not in scored and (i + 2) not in scored:
            gen = methods.make(method_name, w, h)  # restart: history must be real
            continue
        outs = gen.push(frames[r], ts)
        if i == 0 or i not in scored:
            if i == 0 or stride == 1:
                seq_out.append(to_f(frames[r]))
                seq_gt.append(to_f(frames[r]))
            continue
        a, b = to_f(frames[reals[i - 1]]), to_f(frames[r])
        for k, out in enumerate(outs):
            gt = to_f(frames[reals[i - 1] + k + 1])
            out = to_f(out)
            fm = flip_map(gt, out)
            t = ts[k]
            blend = a * (1 - t) + b * t
            hard = (np.abs(luma(blend) - luma(gt)) > 0.06) & ~hud
            per_frame.append(float(fm.mean()))
            hard_sum += float(fm[hard].sum())
            hard_n += float(hard.sum())
            hud_sum += float(fm[hud].sum())
            hud_n += float(hud.sum())
            mse = float(np.mean((luma(out) - luma(gt)) ** 2))
            psnrs.append(99.0 if mse < 1e-12 else 10 * np.log10(1.0 / mse))
            seq_out.append(out)
            seq_gt.append(gt)
            if dump is not None and i in (len(reals) // 2,):
                dump.mkdir(parents=True, exist_ok=True)
                Image.fromarray((out * 255 + 0.5).astype(np.uint8)).save(dump / f"{method_name}_{k}.png")
                Image.fromarray((gt * 255 + 0.5).astype(np.uint8)).save(dump / f"truth_{k}.png")
        seq_out.append(b)
        seq_gt.append(b)
        if stride > 1:  # temporal error within the scored pair only
            temps.extend(float(np.mean(np.abs((luma(seq_out[j]) - luma(seq_out[j - 1]))
                                              - (luma(seq_gt[j]) - luma(seq_gt[j - 1])))))
                         for j in range(len(seq_out) - K, len(seq_out)) if j > 0)
            seq_out, seq_gt = [b], [b]
    if stride == 1:
        temps = [float(np.mean(np.abs((luma(seq_out[j]) - luma(seq_out[j - 1]))
                                      - (luma(seq_gt[j]) - luma(seq_gt[j - 1])))))
                 for j in range(1, len(seq_out))]
    temporal = float(np.mean(temps))
    pf = np.array(per_frame)
    return {
        "flip": float(pf.mean()),
        "flip_hard": hard_sum / max(hard_n, 1.0),
        "flip_hud": hud_sum / max(hud_n, 1.0),
        "psnr": float(np.mean(psnrs)),
        "temporal": temporal,
        "worst": float(np.percentile(pf, 95)),
        "hard_share": hard_n / (len(per_frame) * h * w),
        "frames": len(per_frame),
        "per_frame": per_frame,
        "seconds": time.time() - t0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("--scenes", default="pan,sword,specular,particles,walk,orbit,carry")
    ap.add_argument("--methods", default="repeat,blend,te2")
    ap.add_argument("--pairs", type=int, default=0)
    ap.add_argument("--out", default="")
    ap.add_argument("--dump", default="")
    ap.add_argument("--stride", type=int, default=1, help="score every n-th pair (dev set)")
    args = ap.parse_args()
    root = Path(args.frames)
    results = {}
    cols = ["flip", "flip_hard", "flip_hud", "worst", "temporal", "psnr"]
    print(f"{'scene':10s} {'method':12s} " + " ".join(f"{c:>9s}" for c in cols) + "      sec")
    for s in args.scenes.split(","):
        frames = load_scene(root / s)
        results[s] = {}
        for m in args.methods.split(","):
            r = run_scene(frames, m, args.pairs or None, Path(args.dump) / s if args.dump else None, args.stride)
            results[s][m] = r
            print(f"{s:10s} {m:12s} " + " ".join(f"{r[c]:9.4f}" for c in cols) + f" {r['seconds']:8.1f}",
                  flush=True)
    if args.out:
        Path(args.out).write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()

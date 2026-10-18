"""GPU / reference parity for the LMC path.

Runs te-bench --algo lmc (Vulkan, lavapipe here) and gen_lmc2.LMC2 (numpy) on
the same real frames and compares their generated frames with each other and
with the rendered truth.

usage: python3 parity.py <frames dir> <scene> [--pairs N] [--start S] [--te-bench path]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "reference"))

import bench  # noqa: E402
import methods  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("scene")
    ap.add_argument("--pairs", type=int, default=4)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--te-bench", default=str(HERE.parent / "build" / "te-bench"))
    args = ap.parse_args()
    frames = bench.load_scene(Path(args.frames) / args.scene)
    K = bench.K
    seq = frames[args.start: args.start + K * args.pairs + 1]
    with tempfile.TemporaryDirectory() as td:
        src, gen = os.path.join(td, "seq.npy"), os.path.join(td, "gen.npy")
        np.save(src, seq)
        env = dict(os.environ, VK_ICD_FILENAMES=os.environ.get("TE_ICD", "/usr/share/vulkan/icd.d/lvp_icd.json"))
        subprocess.run([args.te_bench, "--frames", src, "--algo", "lmc", "--step", str(K), "--out", gen,
                        "--validation"], check=True, env=env)
        gpu = np.load(gen)
    ref = methods.make("lmc2", seq.shape[2], seq.shape[1])
    ts = [k / K for k in range(1, K)]
    refs = []
    for i in range(args.pairs + 1):
        refs.extend(ref.push(seq[i * K], ts))
    print(f"{'pair':>4} {'k':>2} {'mean|d|':>8} {'>4/255':>7} {'FLIP gpu-ref':>12} {'FLIP gpu':>9} {'FLIP ref':>9}")
    for j, (g, r) in enumerate(zip(gpu, refs)):
        p, k = j // (K - 1) + 1, j % (K - 1)
        gt = bench.to_f(seq[(p - 1) * K + k + 1])
        gf, rf = bench.to_f(g), bench.to_f(r)
        d = np.abs(gf - rf).max(-1)
        print(f"{p:4d} {k:2d} {d.mean() * 255:8.3f} {100 * (d > 4 / 255).mean():6.2f}% "
              f"{bench.flip_map(rf, gf).mean():12.4f} {bench.flip_map(gt, gf).mean():9.4f} "
              f"{bench.flip_map(gt, rf).mean():9.4f}")


if __name__ == "__main__":
    main()

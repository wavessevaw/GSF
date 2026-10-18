"""Compare te-bench builds on a captured .npy sequence (drop-frame protocol).

Every K-th frame is fed to the generator, the K-1 frames between are
generated and scored with FLIP against the frames that were really there.
Use it to check a change on real footage, not only on the rendered bench.

usage: python3 npy_eval.py frames.npy --k 3 --te-bench a/te-bench --te-bench b/te-bench
       [--start S] [--pairs N] [--segments meta.json]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import bench  # noqa: E402


def run(te_bench, seq, k):
    with tempfile.TemporaryDirectory() as td:
        src, gen, js = (os.path.join(td, n) for n in ("seq.npy", "gen.npy", "stats.jsonl"))
        np.save(src, seq)
        env = dict(os.environ, VK_ICD_FILENAMES=os.environ.get("TE_ICD", "/usr/share/vulkan/icd.d/lvp_icd.json"))
        subprocess.run([te_bench, "--frames", src, "--algo", "lmc", "--step", str(k), "--out", gen, "--json", js],
                       check=True, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = np.load(gen)
        stats = [json.loads(line) for line in open(js)]
    flips = []
    for j, g in enumerate(out):
        p, i = j // (k - 1) + 1, j % (k - 1)
        flips.append(float(bench.flip_map(bench.to_f(seq[(p - 1) * k + i + 1]), bench.to_f(g)).mean()))
    orph = [s.get("lmc_counts", [0, 0, 0])[1:] for s in stats]
    return np.array(flips), orph


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("--k", type=int, default=3)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--pairs", type=int, default=20)
    ap.add_argument("--te-bench", action="append", required=True)
    ap.add_argument("--segments")
    args = ap.parse_args()
    frames = np.load(args.frames, mmap_mode="r")
    segs = [("all", args.start, args.start + args.k * args.pairs + 1)]
    if args.segments:
        segs = [(s["name"], s["start"], min(s["end"], s["start"] + args.k * args.pairs + 1))
                for s in json.load(open(args.segments))["segments"]]
    for name, a, b in segs:
        seq = np.ascontiguousarray(frames[a:b])
        n = (seq.shape[0] - 1) // args.k * args.k + 1
        seq = seq[:n]
        row = [f"{name:12}"]
        for tb in args.te_bench:
            f, orph = run(tb, seq, args.k)
            om = max((max(o) for o in orph if o), default=0)
            row.append(f"flip {f.mean():.4f} p95 {np.percentile(f, 95):.4f} orph_max {om:4d}")
        print(" | ".join(row), flush=True)


if __name__ == "__main__":
    main()

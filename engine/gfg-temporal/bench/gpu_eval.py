"""Quality and cost of the Vulkan LMC path on the bench scenes.

Runs te-bench --algo lmc on each scene (drop-frame protocol, K = 3) and
scores the generated frames against the rendered truth with FLIP.  Also
reports the per-pass GPU times of the device it ran on and the orphan block
counts, so one run answers both "is it still as good" and "what does it
cost".  Software Vulkan times do not predict Deck timing or physical scanout.

usage: python3 gpu_eval.py <frames dir> [--scenes a,b] [--pairs N] [--out results.json]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import bench  # noqa: E402

SCENES = ["pan", "sword", "specular", "particles", "walk", "orbit", "carry"]


def run(frames_dir, scene, pairs, te_bench, algorithm="lmc"):
    frames = bench.load_scene(Path(frames_dir) / scene)
    K = bench.K
    seq = frames[: K * pairs + 1]
    h, w = seq.shape[1:3]
    hud = bench.hud_mask(h, w)
    with tempfile.TemporaryDirectory() as td:
        src, gen, js = (os.path.join(td, n) for n in ("seq.npy", "gen.npy", "stats.jsonl"))
        np.save(src, seq)
        env = dict(os.environ, VK_ICD_FILENAMES=os.environ.get("TE_ICD", "/usr/share/vulkan/icd.d/lvp_icd.json"))
        subprocess.run([te_bench, "--frames", src, "--algo", algorithm, "--step", str(K), "--out", gen, "--json", js,
                        "--tier", os.environ.get("TE_TIER", "0")],
                       check=True, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = np.load(gen)
        stats = [json.loads(line) for line in open(js)]
    flips, huds = [], []
    for j, g in enumerate(out):
        p, k = j // (K - 1) + 1, j % (K - 1)
        gt = bench.to_f(seq[(p - 1) * K + k + 1])
        fm = bench.flip_map(gt, bench.to_f(g))
        flips.append(float(fm.mean()))
        huds.append(float(fm[hud].mean()))
    timed = [s for s in stats if s.get("timed")][1:]  # the first pair compiles and warms caches
    passes = {k: float(np.median([s["lmc_ms"][k] for s in timed])) for k in timed[0]["lmc_ms"]} if timed and "lmc_ms" in timed[0] else {}
    frame = float(np.median([f["total_ms"] for s in timed for f in s["frames"]])) if timed else 0.0
    select = float(np.median([f["lists_ms"] for s in timed for f in s["frames"]])) if timed else 0.0
    pyr = float(np.median([s["pyramid_ms"] for s in timed])) if timed else 0.0
    orph = [s["lmc_counts"][1:] for s in stats if "lmc_counts" in s]
    return {"scene": scene, "algorithm": algorithm, "flip": float(np.mean(flips)), "flip_hud": float(np.mean(huds)),
            "worst": float(np.percentile(flips, 95)), "pyramid": pyr, "passes": passes, "frame": frame,
            "select": select, "orphans_max": int(np.max(orph)) if orph else 0,
            "orphans_mean": float(np.mean(orph)) if orph else 0.0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("--scenes", default=",".join(SCENES))
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--algo",choices=("tiles","lmc","lmc-gpu"),default="lmc")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--out")
    ap.add_argument("--te-bench", default=str(HERE.parent / "build" / "te-bench"))
    args = ap.parse_args()
    scenes = args.scenes.split(",")
    with ThreadPoolExecutor(args.jobs) as ex:
        res = list(ex.map(lambda s: run(args.frames, s, args.pairs, args.te_bench,args.algo), scenes))
    names = list(res[0]["passes"]) if res[0]["passes"] else []
    print(f"{'scene':10} {'flip':>6} {'hud':>6} {'p95':>6} {'orph':>9} {'pyr':>5} "
          + " ".join(f"{n[:7]:>7}" for n in names) + f" {'sel':>5} {'frame':>6} {'ratio':>6}")
    for r in res:
        an = sum(v for k,v in r["passes"].items() if k != "fit_wait")
        ratio = (an + r["pyramid"]) / max(r["pyramid"], 1e-6)
        print(f"{r['scene']:10} {r['flip']:6.4f} {r['flip_hud']:6.4f} {r['worst']:6.4f} "
              f"{r['orphans_mean']:5.0f}/{r['orphans_max']:<4d}{r['pyramid']:5.1f} "
              + " ".join(f"{r['passes'][n]:7.1f}" for n in names)
              + f" {r['select']:5.1f} {r['frame']:6.1f} {ratio:6.1f}")
    if args.out:
        json.dump(res, open(args.out, "w"), indent=1)


if __name__ == "__main__":
    main()

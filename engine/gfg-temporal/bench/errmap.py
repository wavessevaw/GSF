"""Side-by-side FLIP error maps for one generated frame (debug aid).

usage: python3 errmap.py <frames dir> <scene> <pair> <k> <out.png> method [method...]
Each row: output | FLIP error (black = exact, white = 1).
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))

import bench  # noqa: E402
import methods  # noqa: E402


def main():
    root, scene, pair, k, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    names = sys.argv[6:]
    frames = bench.load_scene(Path(root) / scene)
    ts = [j / bench.K for j in range(1, bench.K)]
    rows = []
    gt = bench.to_f(frames[pair * bench.K + k + 1])
    for m in names:
        g = methods.make(m, frames.shape[2], frames.shape[1])
        start = max(0, pair - 3)  # a few pairs of history for temporal methods
        for i in range(start, pair + 2):
            outs = g.push(frames[i * bench.K], ts)
        o = bench.to_f(outs[k])
        fm = bench.flip_map(gt, o)
        print(f"{m}: flip {fm.mean():.4f}")
        e = np.repeat(np.clip(fm, 0, 1)[..., None], 3, -1)
        rows.append(np.concatenate([o, e], 1))
    rows.insert(0, np.concatenate([gt, np.zeros_like(gt)], 1))
    img = (np.concatenate(rows, 0) * 255).astype(np.uint8)
    Image.fromarray(img).resize((img.shape[1] // 2, img.shape[0] // 2)).save(out)


if __name__ == "__main__":
    main()

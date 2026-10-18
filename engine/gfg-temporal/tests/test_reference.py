"""Reference algorithm tests and GPU/reference parity.

Run: python3 -m unittest discover -s engine/gfg-temporal/tests -p 'test_*.py'
The parity test needs a built te-bench (make -C engine/gfg-temporal) and a
Vulkan device (lavapipe in CI); it is skipped, and says so, without them.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ENGINE = HERE.parent
sys.path.insert(0, str(ENGINE / "reference"))

from gfg_temporal_ref import CLASS_FULL, CLASS_LIGHT, CLASS_REUSE, Params, TemporalEngine  # noqa: E402
from gfg_temporal_ref.params import C_HEADER_FIELDS  # noqa: E402
from gfg_temporal_ref.metrics import psnr_y  # noqa: E402

W, H = 256, 160


def pattern(ox, oy, seed=1):
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    u, v = x - ox, y - oy
    s = seed * 0.37
    r = 128 + 60 * np.sin(u * 0.21 + s) * np.cos(v * 0.17) + 40 * np.sin((u + v) * 0.09 + 2 * s)
    g = 128 + 55 * np.sin(v * 0.23 - s) + 45 * np.cos(u * 0.13 + v * 0.05)
    b = 128 + 70 * np.cos(u * 0.07 - v * 0.19 + s)
    return np.clip(np.stack([r, g, b], -1) + 0.5, 0, 255).astype(np.uint8)


def scene(k):
    """Panning background, a box moving the other way, a static corner."""
    f = pattern(4 * k, 1 * k, 2)
    bx = 60 + 9 * k
    f[50:100, bx:bx + 40] = pattern(-2 * k, 0, 5)[50:100, bx:bx + 40]
    f[:40, :48] = 90
    return f


class HeaderSync(unittest.TestCase):
    def test_c_defaults_equal_python_defaults(self):
        text = (ENGINE / "src/te_params.h").read_text()
        p = Params()
        for field, macro in C_HEADER_FIELDS:
            m = re.search(rf"#define {macro} \(?([0-9.]+)f?(?: / ([0-9.]+)f)?\)?", text)
            self.assertIsNotNone(m, macro)
            value = float(m.group(1)) / (float(m.group(2)) if m.group(2) else 1.0)
            self.assertAlmostEqual(value, float(getattr(p, field)), places=6, msg=macro)


class Reference(unittest.TestCase):
    def test_translation_vectors_and_quality(self):
        eng = TemporalEngine()
        eng.push(pattern(0, 0), [])
        outs, rep = eng.push(pattern(6, 2), [0.5])
        v = eng.prev_vectors[1:-1, 1:-1].reshape(-1, 2)
        good = np.mean((np.abs(v[:, 0] - 6) <= 0.5) & (np.abs(v[:, 1] - 2) <= 0.5))
        self.assertGreater(good, 0.9)
        self.assertGreater(psnr_y(outs[0][16:-16, 16:-16], pattern(3, 1)[16:-16, 16:-16]), 34.0)

    def test_static_is_reused_bit_exact(self):
        eng = TemporalEngine()
        f = pattern(0, 0)
        eng.push(f, [])
        outs, rep = eng.push(f, [1 / 3, 2 / 3])
        self.assertTrue(np.all(rep.classes == CLASS_REUSE))
        self.assertTrue(np.array_equal((outs[1] * 255 + 0.5).astype(np.uint8), f))

    def test_scene_cut_holds(self):
        rng = np.random.default_rng(1)
        a = rng.integers(0, 256, (H, W, 3), dtype=np.uint8)
        b = rng.integers(0, 256, (H, W, 3), dtype=np.uint8)
        eng = TemporalEngine()
        eng.push(a, [])
        outs, rep = eng.push(b, [0.25, 0.75])
        self.assertTrue(rep.hold)
        self.assertIsNone(eng.prev_vectors, "history is dropped at a cut")

    def test_classes_mix_and_hysteresis(self):
        eng = TemporalEngine()
        eng.push(scene(0), [])
        seen = set()
        for k in range(1, 5):
            _, rep = eng.push(scene(k), [0.5])
            seen |= set(np.unique(rep.classes).tolist())
        self.assertTrue({CLASS_REUSE, CLASS_LIGHT, CLASS_FULL} <= seen, seen)
        p = Params()
        from gfg_temporal_ref.core import apply_hysteresis
        state = np.array([CLASS_FULL], np.int8)
        count = np.zeros(1, np.int16)
        target = np.array([CLASS_REUSE], np.int8)
        state, count = apply_hysteresis(p, target, state, count)
        self.assertEqual(state[0], CLASS_FULL, "first eligible pair keeps the class")
        state, count = apply_hysteresis(p, target, state, count)
        self.assertEqual(state[0], CLASS_REUSE, "second consecutive eligible pair downgrades")
        state, count = apply_hysteresis(p, np.array([CLASS_FULL], np.int8), state, count)
        self.assertEqual(state[0], CLASS_FULL, "upgrades are immediate")

    def test_experiments_run(self):
        for exp in ("A", "B2x2", "C", "D", "E", "F", "G"):
            eng = TemporalEngine(experiments=[exp])
            eng.push(scene(0), [])
            outs, _ = eng.push(scene(1), [1 / 3, 2 / 3])
            self.assertEqual(len(outs), 2)
            self.assertTrue(np.isfinite(outs[0]).all(), exp)
        with self.assertRaises(ValueError):
            TemporalEngine(experiments=["Z"])


BENCH = Path(os.environ.get("TE_BENCH", ENGINE / "build/te-bench"))


@unittest.skipUnless(BENCH.is_file(), f"te-bench not built at {BENCH}")
class Parity(unittest.TestCase):
    """The GPU engine against the reference on the same frames (tolerances,
    since hardware bilinear filtering and half-float luma differ in the LSBs)."""

    def test_gpu_matches_reference(self):
        frames = np.stack([scene(k) for k in range(0, 13)])
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            np.save(tmp / "frames.npy", frames)
            cmd = [str(BENCH), "--frames", str(tmp / "frames.npy"), "--step", "3", "--out", str(tmp / "gen.npy"),
                   "--analysis", str(tmp / "an"), "--json", str(tmp / "s.jsonl"), "--validation"]
            run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            if run.returncode == 3:
                self.skipTest("no Vulkan device: " + run.stderr.strip())
            self.assertEqual(run.returncode, 0, run.stderr)
            gen = np.load(tmp / "gen.npy")
            meta = json.loads((tmp / "an/meta.json").read_text())
            tx, ty = meta["tiles_x"], meta["tiles_y"]
            gv = np.fromfile(tmp / "an/vectors.f32", np.float32).reshape(-1, ty, tx, 2)
            gc = np.fromfile(tmp / "an/classes.u8", np.uint8).reshape(-1, ty, tx)
        eng = TemporalEngine()
        eng.push(frames[0], [])
        for k in range(4):
            outs, rep = eng.push(frames[3 * (k + 1)], [1 / 3, 2 / 3])
            agree_v = np.mean(np.abs(eng.prev_vectors - gv[k]).max(-1) <= 0.5)
            agree_c = np.mean(rep.per_frame_classes[-1] == gc[k])
            self.assertGreater(agree_v, 0.9, f"pair {k} vectors")
            self.assertGreater(agree_c, 0.9, f"pair {k} classes")
            for i in range(2):
                self.assertGreater(psnr_y(outs[i], gen[2 * k + i]), 35.0, f"pair {k} output {i}")


if __name__ == "__main__":
    unittest.main()

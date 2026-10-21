# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "py_modules"))
import kedavra_ctl  # noqa: E402

KEDAVRA = HERE.parent.parent


class Recorder:
    def __init__(self):
        self.calls = []

    def __call__(self, argv, **kw):
        self.calls.append(argv)
        self.envs = getattr(self, "envs", []) + [kw.get("env")]
        if argv[0].endswith("python3") or argv[0] == sys.executable:
            return subprocess.run(argv, **kw)
        return subprocess.CompletedProcess(argv, 0, "", "")


class ControlTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.now = 1000.0
        self.rec = Recorder()
        plugin = self.tmp / "plugin"
        (plugin / "bin").mkdir(parents=True)
        shutil.copy(KEDAVRA / "tools" / "kfg_pacing.py", plugin / "bin")
        self.ctl = kedavra_ctl.Control(self.tmp / "home", plugin, runner=self.rec, clock=lambda: self.now)
        self.ctl.runtime = self.tmp / "run"
        self.ctl.runtime.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_absent_engine_is_not_reported_running(self):
        s = self.ctl.status()
        self.assertEqual(s["engine"], "absent")
        self.assertFalse(s["fresh"])

    def test_stale_status_is_not_running(self):
        p = self.ctl.runtime / "kedavra-status.json"
        p.write_text(json.dumps({"multiplier": 3, "state": "active"}))
        os.utime(p, (self.now - 10, self.now - 10))
        s = self.ctl.status()
        self.assertEqual(s["engine"], "stale")
        os.utime(p, (self.now - 1, self.now - 1))
        s = self.ctl.status()
        self.assertEqual(s["engine"], "running")
        self.assertEqual(s["multiplier"], 3)

    def test_gamescopectl_does_not_inherit_decky_libraries(self):
        os.environ["LD_LIBRARY_PATH"] = "/tmp/_MEI1234"
        try:
            self.ctl.set_multiplier(2)
        except Exception:
            pass
        finally:
            del os.environ["LD_LIBRARY_PATH"]
        env = self.rec.envs[0]
        self.assertIsNotNone(env)
        self.assertNotIn("LD_LIBRARY_PATH", env)

    def test_multiplier_goes_through_gamescopectl(self):
        self.ctl.set_multiplier(3)
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_multiplier", "3"])
        with self.assertRaises(ValueError):
            self.ctl.set_multiplier(4)

    def test_base_fps_goes_through_gamescopectl(self):
        self.ctl.set_base_fps(40)
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_base_fps", "40"])
        self.ctl.set_base_fps(0)
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_base_fps", "0"])
        with self.assertRaises(ValueError):
            self.ctl.set_base_fps(10)

    def test_base_fps_refused_by_old_engine(self):
        (self.ctl.runtime / "kedavra-status.json").write_text(json.dumps({"engine": "kedavra-gamescope", "auto": False}))
        with self.assertRaises(RuntimeError):
            self.ctl.set_base_fps(30)

    def test_capture_runs_analyzer(self):
        c = self.ctl.start_capture("x3 test")
        self.assertEqual(self.rec.calls[-1][-2], "kfg_trace")
        # Fake a perfectly paced x3 trace at 90 Hz as the engine would write it.
        period = 11111111
        rows = ["vblank,flip_ns,kind,src_a,src_b,phase_num,phase_den,content,delay,reason,state,refresh_ns,mult,compose_late,skipped"]
        for k in range(90 * 20):
            src = k // 3
            ph = k % 3
            kind = "real" if ph == 0 else "gen"
            rows.append(f"{k},{k * period},{kind},{src},{src + 1 if ph else 0},{ph},{3 if ph else 0},{k},3,none,active,{period},3,0,0")
        Path(c["path"]).write_text("\n".join(rows) + "\n")
        r = self.ctl.stop_capture()
        self.assertEqual(self.rec.calls[-2][-2:], ["kfg_trace", ""])
        self.assertIn("summary", r, r["log"])
        self.assertLess(r["summary"]["p95_ms"], 0.01)
        self.assertIsNone(self.ctl.capture)


if __name__ == "__main__":
    unittest.main()

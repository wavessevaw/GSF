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
        # The engine still reports the file open for a moment after the stop.
        status = self.ctl.runtime / "kedavra-status.json"
        status.write_text(json.dumps({"engine": "kedavra-gamescope", "trace": c["path"]}))
        naps = []

        def nap(s):
            naps.append(s)
            status.write_text(json.dumps({"engine": "kedavra-gamescope", "trace": ""}))
        self.ctl.sleep = nap
        r = self.ctl.stop_capture()
        self.assertEqual(len(naps), 1)
        self.assertEqual(self.rec.calls[-2][-2:], ["kfg_trace", ""])
        self.assertIn("summary", r, r["log"])
        self.assertLess(r["summary"]["p95_ms"], 0.01)
        self.assertIsNone(self.ctl.capture)

    def test_options_x375_and_hdr(self):
        status = self.ctl.runtime / "kedavra-status.json"
        status.write_text(json.dumps({"engine": "kedavra-gamescope", "req_auto_floor": 0, "hdr_enabled": True}))
        self.assertFalse(self.ctl.options_differ(json.loads(status.read_text()), {"x375": False, "hdr": True}))
        self.assertTrue(self.ctl.options_differ(json.loads(status.read_text()), {"x375": True, "hdr": True}))
        self.ctl.apply_options({"x375": True, "hdr": False})
        self.assertEqual(self.rec.calls[-2][-2:], ["kfg_auto_floor", "20"])
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_hdr", "0"])

    def test_options_refused_by_old_engine(self):
        (self.ctl.runtime / "kedavra-status.json").write_text(json.dumps({"engine": "kedavra-gamescope", "auto": True}))
        self.assertFalse(self.ctl.options_differ({"auto": True}, {"x375": True, "hdr": True}))
        with self.assertRaises(RuntimeError):
            self.ctl.apply_options({"x375": True, "hdr": True})

    def render_status(self, **changes):
        st = {"experimental_build": kedavra_ctl.C1_BUILD,
              "post_direct_history_requested": False, "post": ""}
        st.update(changes)
        path = self.ctl.runtime / "kedavra-status.json"
        path.write_text(json.dumps(st))
        os.utime(path, (self.now, self.now))
        return self.ctl.status()

    def test_render_presets_and_c1_on_off(self):
        self.render_status()
        self.ctl.apply_render_options({"post_preset": "sharp", "post_direct_history": True})
        self.assertEqual(self.rec.calls[-2][-2:], ["kfg_post", "cas=0.5"])
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_post_direct_history", "1"])
        self.ctl.apply_render_options({"post_preset": "off", "post_direct_history": False})
        self.assertEqual(self.rec.calls[-2][-2:], ["kfg_post", ""])
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_post_direct_history", "0"])

    def test_hardware_runtime_options_guard_and_apply(self):
        self.render_status()
        with self.assertRaises(RuntimeError):
            self.ctl.apply_runtime_options({"direct_scan": True, "deadline_analysis": True})
        self.assertEqual(self.rec.calls, [])
        st = self.render_status(hardware_runtime_version=1, direct_scan_requested=False, deadline_analysis_requested=False)
        options = {"direct_scan": True, "deadline_analysis": True}
        self.assertTrue(self.ctl.runtime_options_differ(st, options))
        self.ctl.apply_runtime_options(options)
        self.assertEqual(self.rec.calls[-2][-2:], ["kfg_direct_scan", "1"])
        self.assertEqual(self.rec.calls[-1][-2:], ["kfg_deadline_analysis", "1"])
        with self.assertRaises(ValueError):
            self.ctl.apply_runtime_options({"direct_scan": "true"})

    def test_render_options_validate_before_sending(self):
        self.render_status()
        for settings in ({"post_preset": "cas=999; bad"}, {"post_direct_history": "true"}):
            with self.assertRaises(ValueError):
                self.ctl.apply_render_options(settings)
        self.assertEqual(self.rec.calls, [])

    def test_c1_refuses_old_or_stale_runtime(self):
        for changes in ({"experimental_build": "old"}, {"post_direct_history_requested": None}):
            self.render_status(**changes)
            with self.assertRaises(RuntimeError):
                self.ctl.apply_render_options({"post_preset": "sharp", "post_direct_history": True})
        self.render_status()
        p = self.ctl.runtime / "kedavra-status.json"
        os.utime(p, (self.now - 10, self.now - 10))
        with self.assertRaises(RuntimeError):
            self.ctl.apply_render_options({"post_direct_history": True})
        self.assertEqual(self.rec.calls, [])

    def test_render_options_restore_after_restart_without_sending_c1_to_old_engine(self):
        settings = {"post_preset": "smooth", "post_direct_history": True}
        st = self.render_status()
        self.assertTrue(self.ctl.render_options_differ(st, settings))
        self.ctl.apply_render_options(settings, strict=False)
        st = self.render_status(post="cas=0.3,smaa", post_direct_history_requested=True)
        self.assertFalse(self.ctl.render_options_differ(st, settings))
        st = self.render_status(experimental_build="old", post="cas=0.3,smaa")
        self.assertFalse(self.ctl.render_options_differ(st, settings))
        self.rec.calls.clear()
        self.ctl.apply_render_options(settings, strict=False)
        self.assertEqual([c[-2] for c in self.rec.calls], ["kfg_post"])


if __name__ == "__main__":
    unittest.main()

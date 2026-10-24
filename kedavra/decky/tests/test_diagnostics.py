# SPDX-License-Identifier: GPL-3.0-or-later
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "py_modules"))
from kedavra_diagnostics import Diagnostics


class DiagnosticsTest(unittest.TestCase):
    def test_rotation_is_bounded_and_records_are_json(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Diagnostics(directory, limit=1024)
            for i in range(50):
                self.assertTrue(log.event("failure", "error", code=i, message="reason" * 20))
            files = list(Path(directory).glob("runtime-events.jsonl*"))
            self.assertEqual(len(files), 3)
            for file in files:
                self.assertLess(file.stat().st_size, 1024)
                for line in file.read_text().splitlines():
                    event = json.loads(line)
                    self.assertEqual(event["level"], "error")
                    self.assertIn("time_utc", event)
                    self.assertIn("session", event)

    def test_status_fallback_error_restart_and_throttle(self):
        with tempfile.TemporaryDirectory() as directory:
            now = [1000.0]
            log = Diagnostics(directory, clock=lambda: now[0])
            state = {"fresh": True, "engine": "running", "time_ns": 500,
                     "direct_scan_reason": "atomic_plane_validation_failed"}
            log.observe(state)
            for _ in range(20): log.observe(state)
            self.assertEqual(len(log.path.read_text().splitlines()), 1)
            log.observe({**state, "time_ns": 600, "post_error": "bad preset"})
            log.observe({**state, "time_ns": 1})
            records = [json.loads(line) for line in log.path.read_text().splitlines()]
            self.assertEqual(records[1]["level"], "error")
            self.assertEqual(records[2]["event"], "runtime_restart")
            self.assertEqual(records[0]["status"]["direct_scan_reason"], "atomic_plane_validation_failed")

    def test_unwritable_storage_exposes_failure_without_crashing(self):
        with tempfile.TemporaryDirectory() as directory:
            blocked = Path(directory) / "file"
            blocked.write_text("not a directory")
            log = Diagnostics(blocked)
            self.assertFalse(log.event("error"))
            self.assertTrue(log.summary()["logging_error"])

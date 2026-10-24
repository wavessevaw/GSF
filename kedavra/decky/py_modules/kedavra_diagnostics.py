# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded persistent runtime evidence. File I/O runs in Decky workers, never GPU/KMS threads."""
from datetime import datetime, timezone
import json
from pathlib import Path
import threading
import time
import uuid


class Diagnostics:
    FIELDS = ("fresh", "engine", "time_ns", "engine_pid", "trace_dropped", "experimental_build", "hardware_runtime_version", "history_epoch",
              "app_id", "state", "width", "height", "transfer", "steam_ui", "post", "post_error", "last_problem",
              "post_direct_history_requested", "c1_reason", "post_direct_history_sdr_records", "post_history_copy_records",
              "direct_scan_requested", "direct_scan_reason", "direct_scan_prepared", "direct_scan_commits",
              "direct_scan_flips", "direct_scan_failures", "direct_scan_present_id", "direct_scan_fb", "direct_scan_flip_ns",
              "deadline_analysis_requested", "analysis_reason", "analysis_completions", "analysis_deferred",
              "analysis_dropped", "analysis_jobs_pending", "analysis_submit_cpu_ns", "analysis_completion_observed_ns",
              "gpu_us_last", "gpu_analysis_us_last", "final_cpu_record_wait_ns", "over_budget", "ring_miss", "unsupported")

    def __init__(self, directory, clock=time.time, limit=512 * 1024):
        self.directory, self.clock, self.limit = Path(directory), clock, limit
        self.path = self.directory / "runtime-events.jsonl"
        self.session = uuid.uuid4().hex
        self.lock = threading.RLock()
        self.last_error = ""
        self.previous = None
        self.last_snapshot = 0.0

    def event(self, event, level="info", **details):
        record = {"time_utc": datetime.fromtimestamp(self.clock(), timezone.utc).isoformat(),
                  "backend_monotonic_ns": time.monotonic_ns(), "session": self.session,
                  "event": event, "level": level, **details}
        try:
            line = json.dumps(record, ensure_ascii=False, default=str)[:16000]
            # Do not truncate JSON in the middle of a string.
            if len(line) >= 16000:
                line = json.dumps({**record, "details_truncated": True}, default=lambda _: "<omitted>")
                if len(line) > 16000:
                    line = json.dumps({k: v for k, v in record.items() if k in ("time_utc", "session", "event", "level")})
            with self.lock:
                self.directory.mkdir(parents=True, exist_ok=True)
                if self.path.exists() and self.path.stat().st_size + len(line.encode("utf-8")) > self.limit:
                    older = self.path.with_name(self.path.name + ".2")
                    older.unlink(missing_ok=True)
                    previous = self.path.with_name(self.path.name + ".1")
                    if previous.exists(): previous.replace(older)
                    self.path.replace(previous)
                with self.path.open("a", encoding="utf-8") as output:
                    output.write(line + "\n")
                self.path.chmod(0o600)
                self.last_error = ""
            return True
        except (OSError, ValueError) as error:
            self.last_error = str(error)[:500]
            return False

    def observe(self, status):
        current = {key: status.get(key) for key in self.FIELDS}
        previous = self.previous
        restart = bool(previous and ((current.get("engine_pid") and previous.get("engine_pid")
                       and current["engine_pid"] != previous["engine_pid"])
                       or (current.get("time_ns") and previous.get("time_ns") and current["time_ns"] < previous["time_ns"])))
        transition = not previous or restart or any(current.get(k) != previous.get(k) for k in (
            "fresh", "engine", "experimental_build", "history_epoch", "state", "transfer", "post",
            "post_direct_history_requested", "c1_reason", "direct_scan_requested", "direct_scan_reason",
            "deadline_analysis_requested", "post_error", "last_problem"))
        if transition or self.clock() - self.last_snapshot >= 5:
            level = "warning" if not current.get("fresh") else "error" if current.get("post_error") else "info"
            self.event("runtime_restart" if restart else "runtime_status", level, sampled=True, status=current)
            self.last_snapshot = self.clock()
        self.previous = current

    def tail(self):
        with self.lock:
            try:
                with self.path.open("rb") as source:
                    source.seek(max(0, self.path.stat().st_size - 12000))
                    return source.read().decode("utf-8", errors="replace")
            except OSError:
                return ""

    def summary(self):
        return {"logging_error": self.last_error, "log": str(self.path), "session": self.session,
                "status": self.previous, "sampled_status": True, "max_files": 3, "max_file_bytes": self.limit}

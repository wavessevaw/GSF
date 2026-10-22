# SPDX-License-Identifier: GPL-3.0-or-later
"""KEDAVRA control backend for the Decky plugin.

The engine runs inside the patched Gamescope. This module only:
  * sets Gamescope convars with gamescopectl (kfg_multiplier, kfg_base_fps, kfg_trace);
  * reads the status file the engine rewrites every second, and reports the
    engine as running only while that file is fresh (never from settings alone);
  * starts and stops KMS-flip trace captures and analyses them with kfg_pacing.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from kedavra_install import clean_env

STATUS_STALE_S = 3.0


class Control:
    def __init__(self, home, plugin_dir, uid=1000, runner=subprocess.run, clock=time.time, sleep=time.sleep):
        self.home = Path(home)
        self.plugin_dir = Path(plugin_dir)
        self.runtime = Path(f"/run/user/{uid}")
        self.data = self.home / ".local/share/kedavra"
        self.captures = self.data / "captures"
        self.run = runner
        self.clock = clock
        self.sleep = sleep
        self.capture = None  # {"path": str, "started": float, "mult": int}
        self.uid = uid
        # The KEDAVRA gamescopectl speaks the same protocol; prefer it once installed.
        self.ctl_candidates = [Path("/var/lib/kedavra/runtime/bin/gamescopectl")]

    # ------------------------------------------------------------ gamescopectl

    def _env(self):
        env = clean_env()
        env["XDG_RUNTIME_DIR"] = str(self.runtime)
        env.setdefault("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0")
        return env

    def _ctl(self, *args):
        exe = next((str(p) for p in self.ctl_candidates if p.exists()), None) or shutil.which("gamescopectl") or "gamescopectl"
        r = self.run([exe, *map(str, args)], env=self._env(), capture_output=True, text=True, timeout=5)
        if r.returncode != 0:
            raise RuntimeError(f"gamescopectl {' '.join(map(str, args))}: {(r.stderr or r.stdout).strip() or r.returncode}")
        return r.stdout

    # ------------------------------------------------------------ status

    def status(self):
        path = self.runtime / "kedavra-status.json"
        out = {"engine": "absent", "fresh": False, "capture": self._capture_info()}
        try:
            raw = path.read_text()
            st = json.loads(raw)
            age = self.clock() - path.stat().st_mtime
        except (OSError, ValueError):
            out["message"] = "Gamescope KEDAVRA is not running: no engine status (stock Gamescope, or the engine is not installed)."
            return out
        out.update(st)
        out["age_s"] = round(age, 1)
        out["fresh"] = age <= STATUS_STALE_S
        out["engine"] = "running" if out["fresh"] else "stale"
        if not out["fresh"]:
            out["message"] = "Engine status is stale: Gamescope KEDAVRA does not respond."
        return out

    def set_multiplier(self, mult):
        mult = int(mult)
        if mult not in (0, 1, 2, 3):
            raise ValueError("multiplier must be 0 (off), 1 (auto), 2 or 3")
        if mult == 1:
            st = self.status()
            if st.get("fresh") and "auto" not in st:
                # A Gamescope from before auto mode reads 1 as "off".
                raise RuntimeError("The installed Gamescope KEDAVRA is older than the plugin and has no Auto mode: "
                                   "press Reinstall Gamescope KEDAVRA and reboot the Deck.")
        self._ctl("kfg_multiplier", mult)
        return {"multiplier": mult}

    def set_base_fps(self, fps):
        """Base frame cap the engine forces on the game before generation; 0 = automatic."""
        fps = int(fps)
        if fps != 0 and not 20 <= fps <= 240:
            raise ValueError("base fps must be 0 (auto) or 20..240")
        st = self.status()
        if st.get("fresh") and "base_fps" not in st:
            raise RuntimeError("The installed Gamescope KEDAVRA is older than the plugin and has no base FPS: "
                               "press Reinstall Gamescope KEDAVRA and reboot the Deck.")
        self._ctl("kfg_base_fps", fps)
        return {"base_fps": fps}

    # The lowest floor Auto may use; the engine raises it to refresh / 3.75 (24 at 90 Hz).
    X375_FLOOR = 20

    def options_differ(self, st, settings):
        """True when a running engine's options are not the plugin's (it restarted with defaults)."""
        if "req_auto_floor" not in st:
            return False  # engine without these options
        want_floor = self.X375_FLOOR if settings.get("x375") else 0
        return st.get("req_auto_floor") != want_floor or bool(st.get("hdr_enabled")) != bool(settings.get("hdr"))

    def apply_options(self, settings):
        st = self.status()
        if st.get("fresh") and "req_auto_floor" not in st:
            raise RuntimeError("The installed Gamescope KEDAVRA is older than the plugin and has no x3.75 or HDR: "
                               "press Reinstall Gamescope KEDAVRA and reboot the Deck.")
        self._ctl("kfg_auto_floor", self.X375_FLOOR if settings.get("x375") else 0)
        self._ctl("kfg_hdr", 1 if settings.get("hdr") else 0)
        return {"x375": bool(settings.get("x375")), "hdr": bool(settings.get("hdr"))}

    # ------------------------------------------------------------ capture

    def _capture_info(self):
        if not self.capture:
            return None
        return {**self.capture, "elapsed_s": round(self.clock() - self.capture["started"], 1)}

    def start_capture(self, label="run"):
        if self.capture:
            raise RuntimeError("capture already running")
        self.captures.mkdir(parents=True, exist_ok=True)
        # Gamescope runs as the user and writes the trace itself.
        for d in (self.data, self.captures):
            try:
                os.chown(d, self.uid, self.uid)
            except OSError:
                pass
        stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime(self.clock()))
        safe = "".join(c for c in str(label) if c.isalnum() or c in "-_")[:40] or "run"
        path = self.captures / f"{stamp}-{safe}.csv"
        self._ctl("kfg_trace", str(path))
        self.capture = {"path": str(path), "started": self.clock(), "label": safe}
        return self._capture_info()

    def stop_capture(self):
        if not self.capture:
            raise RuntimeError("no capture running")
        cap = self.capture
        self.capture = None
        self._ctl("kfg_trace", "")
        # Gamescope closes the file on its trace thread and then drops it from the status
        # (rewritten once a second): analyse only after that, so the last rows are in.
        for _ in range(30):
            if self.status().get("trace") != cap["path"]:
                break
            self.sleep(0.1)
        return self.analyze(cap["path"])

    def analyze(self, path):
        tool = self.plugin_dir / "bin" / "kfg_pacing.py"
        out = Path(path).with_suffix(".json")
        r = self.run([sys.executable, str(tool), str(path), "--origin", "kms_flip_observed", "--json", str(out)],
                     capture_output=True, text=True, timeout=120)
        report = {"trace": str(path), "report": str(out), "ok": r.returncode == 0, "log": (r.stdout + r.stderr)[-2000:]}
        try:
            data = json.loads(out.read_text())
            report["summary"] = {
                "pass": data.get("pass"),
                "p95_ms": data.get("interval_error_ms", {}).get("p95"),
                "p99_ms": data.get("interval_error_ms", {}).get("p99"),
                "missed_pct": 100.0 * (data.get("missed_fraction") or 0.0),
                "rows": data.get("rows"),
                "checks": data.get("checks"),
            }
        except (OSError, ValueError):
            pass
        return report

    def list_captures(self):
        if not self.captures.exists():
            return []
        return sorted((p.name for p in self.captures.glob("*.csv")), reverse=True)[:20]

    def logs(self, lines=80):
        r = self.run(["journalctl", "-b", "--no-pager", "-n", str(int(lines)), "-g", "kfg|KEDAVRA|gamescope"],
                     capture_output=True, text=True, timeout=10)
        return (r.stdout or r.stderr)[-8000:]

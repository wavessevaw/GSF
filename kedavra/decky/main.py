# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
import time
import json
from pathlib import Path

import decky
from kedavra_ctl import Control
from kedavra_hud import Hud
from kedavra_install import Installer, bench, runtime_selftest
from kedavra_diagnostics import Diagnostics


class Plugin:
    async def _main(self):
        here = Path(__file__).resolve().parent
        self.ctl = Control(decky.DECKY_USER_HOME, here)
        self.inst = Installer(here, decky.DECKY_USER_HOME)
        self.out = Path(decky.DECKY_USER_HOME) / ".local/share/kedavra"
        self.lock = asyncio.Lock()
        self.settings_path = self.out / "settings.json"
        self.settings = self._load_settings()
        self.diagnostics = Diagnostics(self.out)
        self.diagnostics.event("plugin_started", settings=self.settings, build=self.inst.build_info(here / "runtime"))
        self.options_at = 0.0
        self.hud = Hud()
        loop = asyncio.get_event_loop()
        self.health = loop.create_task(self._health())
        self.hud_task = loop.create_task(self._hud())

    DEFAULTS = {"hud": True, "base_fps": 30, "x375": False, "hdr": True,
                "post_preset": "off", "post_direct_history": False, "direct_scan": False, "deadline_analysis": False}

    def _load_settings(self):
        try:
            settings = {**self.DEFAULTS, **json.loads(self.settings_path.read_text())}
            # A damaged/older settings file must never turn an experiment on.
            settings["post_direct_history"] = settings.get("post_direct_history") is True
            for key in ("direct_scan", "deadline_analysis"):
                settings[key] = settings.get(key) is True
            try:
                self.ctl.render_options(settings)
            except (ValueError, TypeError):
                settings["post_preset"] = "off"
            return settings
        except (OSError, ValueError, TypeError):
            return dict(self.DEFAULTS)

    def _save_settings(self):
        try:
            self.out.mkdir(parents=True, exist_ok=True)
            self.settings_path.write_text(json.dumps(self.settings))
        except OSError as e:
            decky.logger.warning(f"settings: {e}")
            self._event("settings_save_failed", "error", error=str(e))
            raise

    async def _hud(self):
        # The on-screen HUD: rendered here once a second, drawn by the KEDAVRA Gamescope.
        while True:
            try:
                st = await asyncio.to_thread(self.ctl.status)
                await asyncio.to_thread(self.diagnostics.observe, st)
                await asyncio.to_thread(self.hud.tick, bool(self.settings.get("hud")), st)
                # A restarted Gamescope comes back with its defaults: restore the engine options.
                # (The status file is rewritten once a second, so wait a few before checking again.)
                if st.get("fresh") and time.monotonic() - self.options_at > 3 and self.ctl.options_differ(st, self.settings):
                    self.options_at = time.monotonic()
                    async with self.lock:
                        await asyncio.to_thread(self.ctl.apply_options, self.settings)
                if st.get("fresh") and self.ctl.render_options_differ(st, self.settings):
                    async with self.lock:
                        await asyncio.to_thread(self.ctl.apply_render_options, self.settings, False)
                if self.ctl.runtime_options_differ(st, self.settings):
                    async with self.lock:
                        self._event("runtime_options_restored", settings=self.ctl.runtime_options(self.settings))
                        await asyncio.to_thread(self.ctl.apply_runtime_options, self.settings, False)
            except Exception as e:
                decky.logger.warning(f"hud: {e}")
                self._event("background_apply_failed", "error", error=str(e)[:1500])
            await asyncio.sleep(1)

    async def _health(self):
        # A boot that mounted KEDAVRA is confirmed only once its Gamescope writes status;
        # an unconfirmed boot is rolled back on the next boot.
        while True:
            try:
                await asyncio.to_thread(self.inst.mark_healthy)
            except Exception as e:
                decky.logger.warning(f"health check: {e}")
            await asyncio.sleep(5)

    async def _unload(self):
        self.health.cancel()
        self.hud_task.cancel()
        await asyncio.to_thread(self.hud.tick, False, None)

    async def _uninstall(self):
        try:
            await asyncio.to_thread(self.ctl.set_multiplier, 0)
        except Exception:
            pass
        await asyncio.to_thread(self.inst.uninstall)

    async def get_hud(self):
        return {"hud": bool(self.settings.get("hud"))}

    async def set_hud(self, on: bool):
        self.settings["hud"] = bool(on)
        await asyncio.to_thread(self._save_settings)
        return {"hud": self.settings["hud"]}

    async def get_settings(self):
        return {"hud": bool(self.settings.get("hud")), "base_fps": int(self.settings.get("base_fps") or 30),
                "x375": bool(self.settings.get("x375")), "hdr": bool(self.settings.get("hdr")),
                **self.ctl.render_options(self.settings), **self.ctl.runtime_options(self.settings)}

    def _event(self, event, level="info", **details):
        diagnostic = getattr(self, "diagnostics", None)
        if diagnostic:
            diagnostic.event(event, level, **details)

    async def set_runtime_options(self, direct_scan: bool, deadline_analysis: bool):
        async with self.lock:
            previous = dict(self.settings)
            proposed = {**previous, "direct_scan": direct_scan, "deadline_analysis": deadline_analysis}
            self.ctl.runtime_options(proposed)
            self._event("runtime_options_requested", settings=self.ctl.runtime_options(proposed))
            try:
                result = await asyncio.to_thread(self.ctl.apply_runtime_options, proposed)
            except Exception as error:
                self._event("runtime_options_failed", "error", error=str(error)[:1500])
                try:
                    await asyncio.to_thread(self.ctl.apply_runtime_options, previous, False)
                    self._event("runtime_options_rollback")
                except Exception as rollback:
                    self._event("runtime_options_rollback_failed", "error", error=str(rollback)[:1500])
                raise
            self.settings = proposed
            try:
                await asyncio.to_thread(self._save_settings)
            except OSError:
                self.settings = previous
                await asyncio.to_thread(self.ctl.apply_runtime_options, previous, False)
                self._event("runtime_options_rollback_after_save_failure", "error")
                raise
            self._event("runtime_options_applied", settings=result)
            return result

    async def set_render_options(self, post_preset: str, post_direct_history: bool):
        """Image filters and C1 are controlled and persisted from the plugin."""
        async with self.lock:
            previous = dict(self.settings)
            proposed = {**previous, "post_preset": post_preset, "post_direct_history": post_direct_history}
            self.ctl.render_options(proposed)  # validate before any command or settings mutation
            self._event("c1_options_requested", settings=self.ctl.render_options(proposed))
            try:
                result = await asyncio.to_thread(self.ctl.apply_render_options, proposed)
            except Exception as error:
                self._event("c1_options_failed", "error", error=str(error)[:1500])
                # A second command may have failed after changing the filters.
                try:
                    await asyncio.to_thread(self.ctl.apply_render_options, previous, False)
                    self._event("c1_options_rollback")
                except Exception as rollback:
                    self._event("c1_options_rollback_failed", "error", error=str(rollback)[:1500])
                raise
            self.settings = proposed
            try:
                await asyncio.to_thread(self._save_settings)
            except OSError:
                self.settings = previous
                await asyncio.to_thread(self.ctl.apply_render_options, previous, False)
                self._event("c1_options_rollback_after_save_failure", "error")
                raise
            self._event("c1_options_applied", settings=result)
            return result

    async def set_options(self, x375: bool, hdr: bool):
        """Engine options: Auto may go down to its low floor (x3.75 at 90 Hz, x4 at 120, x2.5 at 60); generate for HDR games."""
        async with self.lock:
            self.settings["x375"] = bool(x375)
            self.settings["hdr"] = bool(hdr)
            await asyncio.to_thread(self._save_settings)
            self.options_at = time.monotonic()
            await asyncio.to_thread(self.ctl.apply_options, self.settings)
            return {"x375": self.settings["x375"], "hdr": self.settings["hdr"]}

    async def set_mode(self, mode: str, base_fps: int = 0):
        """off | auto | cap (the game is held at base_fps, generation fills the refresh)."""
        if mode not in ("off", "auto", "cap"):
            raise ValueError(f"unknown mode {mode}")
        async with self.lock:
            if mode == "cap":
                self.settings["base_fps"] = int(base_fps)
                await asyncio.to_thread(self._save_settings)
                await asyncio.to_thread(self.ctl.set_base_fps, int(base_fps))
            elif mode == "auto":
                await asyncio.to_thread(self.ctl.set_base_fps, 0)
            await asyncio.to_thread(self.ctl.set_multiplier, 0 if mode == "off" else 1)
            return {"mode": mode, "base_fps": int(base_fps) if mode == "cap" else 0}

    async def get_status(self):
        st = await asyncio.to_thread(self.ctl.status)
        st["live"] = self.hud.live  # rates from the once-a-second HUD loop
        st["diagnostic_logging_error"] = self.diagnostics.last_error
        return st

    async def set_multiplier(self, mult: int):
        async with self.lock:
            return await asyncio.to_thread(self.ctl.set_multiplier, mult)

    async def start_capture(self, label: str):
        async with self.lock:
            return await asyncio.to_thread(self.ctl.start_capture, label)

    async def stop_capture(self):
        async with self.lock:
            return await asyncio.to_thread(self.ctl.stop_capture)

    async def list_captures(self):
        return await asyncio.to_thread(self.ctl.list_captures)

    async def get_logs(self):
        return "RUNTIME EVENTS\n" + await asyncio.to_thread(self.diagnostics.tail) + "\nSYSTEM JOURNAL\n" + await asyncio.to_thread(self.ctl.logs)

    async def diagnose(self):
        return await asyncio.to_thread(self.inst.diagnose)

    async def save_report(self):
        await asyncio.to_thread(self.diagnostics.observe, await asyncio.to_thread(self.ctl.status))
        await asyncio.to_thread((self.out / "runtime-summary.json").write_text,
                                json.dumps(self.diagnostics.summary(), ensure_ascii=False, indent=2))
        return await asyncio.to_thread(self.inst.report, self.out)

    async def install(self):
        async with self.lock:
            return await asyncio.to_thread(self.inst.install)

    async def uninstall(self):
        async with self.lock:
            return await asyncio.to_thread(self.inst.uninstall)

    async def reboot(self):
        return await asyncio.to_thread(self.inst.reboot)

    async def run_bench(self):
        async with self.lock:
            return await asyncio.to_thread(bench, Path(__file__).resolve().parent, self.out)

    async def run_runtime_selftest(self):
        async with self.lock:
            self._event("selftest_started")
            result = await asyncio.to_thread(runtime_selftest, Path(__file__).resolve().parent, self.out)
            self._event("selftest_finished", "info" if result["ok"] else "error", result={k: v for k, v in result.items() if k != "log"})
            return result

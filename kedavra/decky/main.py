# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
import json
from pathlib import Path

import decky
from kedavra_ctl import Control
from kedavra_hud import Hud
from kedavra_install import Installer, bench


class Plugin:
    async def _main(self):
        here = Path(__file__).resolve().parent
        self.ctl = Control(decky.DECKY_USER_HOME, here)
        self.inst = Installer(here, decky.DECKY_USER_HOME)
        self.out = Path(decky.DECKY_USER_HOME) / ".local/share/kedavra"
        self.lock = asyncio.Lock()
        self.settings_path = self.out / "settings.json"
        self.settings = self._load_settings()
        self.hud = Hud()
        loop = asyncio.get_event_loop()
        self.health = loop.create_task(self._health())
        self.hud_task = loop.create_task(self._hud())

    DEFAULTS = {"hud": True, "base_fps": 30}

    def _load_settings(self):
        try:
            return {**self.DEFAULTS, **json.loads(self.settings_path.read_text())}
        except (OSError, ValueError):
            return dict(self.DEFAULTS)

    def _save_settings(self):
        try:
            self.out.mkdir(parents=True, exist_ok=True)
            self.settings_path.write_text(json.dumps(self.settings))
        except OSError as e:
            decky.logger.warning(f"settings: {e}")

    async def _hud(self):
        # The on-screen HUD: rendered here once a second, drawn by the KEDAVRA Gamescope.
        while True:
            try:
                st = await asyncio.to_thread(self.ctl.status)
                await asyncio.to_thread(self.hud.tick, bool(self.settings.get("hud")), st)
            except Exception as e:
                decky.logger.warning(f"hud: {e}")
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
        return {"hud": bool(self.settings.get("hud")), "base_fps": int(self.settings.get("base_fps") or 30)}

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
        return await asyncio.to_thread(self.ctl.logs)

    async def diagnose(self):
        return await asyncio.to_thread(self.inst.diagnose)

    async def save_report(self):
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

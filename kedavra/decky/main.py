# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
from pathlib import Path

import decky
from kedavra_ctl import Control
from kedavra_install import Installer, bench


class Plugin:
    async def _main(self):
        here = Path(__file__).resolve().parent
        self.ctl = Control(decky.DECKY_USER_HOME, here)
        self.inst = Installer(here, decky.DECKY_USER_HOME)
        self.out = Path(decky.DECKY_USER_HOME) / ".local/share/kedavra"
        self.lock = asyncio.Lock()
        self.health = asyncio.get_event_loop().create_task(self._health())

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

    async def _uninstall(self):
        try:
            await asyncio.to_thread(self.ctl.set_multiplier, 0)
        except Exception:
            pass
        await asyncio.to_thread(self.inst.uninstall)

    async def get_status(self):
        return await asyncio.to_thread(self.ctl.status)

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

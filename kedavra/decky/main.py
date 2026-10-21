# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
from pathlib import Path

import decky
from kedavra_ctl import Control


class Plugin:
    async def _main(self):
        self.ctl = Control(decky.DECKY_USER_HOME, Path(__file__).resolve().parent)
        self.lock = asyncio.Lock()

    async def _unload(self):
        pass

    async def _uninstall(self):
        try:
            await asyncio.to_thread(self.ctl.set_multiplier, 0)
        except Exception:
            pass

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

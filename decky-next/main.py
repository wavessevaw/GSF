# SPDX-License-Identifier: GPL-3.0-or-later
import asyncio
from pathlib import Path
import decky
from next_runtime import Runtime

class Plugin:
    async def _main(self):
        self.runtime=Runtime(decky.DECKY_USER_HOME,Path(__file__).resolve().parent)
        self.lock=asyncio.Lock()
        await asyncio.to_thread(self.runtime.install)
    async def _unload(self): pass
    async def _uninstall(self):
        await asyncio.to_thread(self.runtime.disable_all)
    async def get_games(self):
        return await asyncio.to_thread(self.runtime.games)
    async def get_selection(self):
        return await asyncio.to_thread(self.runtime.selection)
    async def select_game(self,appid:int):
        async with self.lock: return await asyncio.to_thread(self.runtime.select,appid)
    async def prepare_game(self,appid:int):
        async with self.lock: return await asyncio.to_thread(self.runtime.prepare,appid)
    async def configure_game(self,appid:int,patch:dict):
        async with self.lock: return await asyncio.to_thread(self.runtime.configure,appid,patch)
    async def get_status(self,appid:int):
        return await asyncio.to_thread(self.runtime.status,appid)
    async def export_report(self,appid:int):
        return await asyncio.to_thread(self.runtime.export,appid)

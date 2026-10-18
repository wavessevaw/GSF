import asyncio
from pathlib import Path
import decky
from gsf_runtime import Runtime


class Plugin:
    async def _main(self):
        self.runtime = Runtime(decky.DECKY_USER_HOME, Path(__file__).resolve().parent)
        self.lock = asyncio.Lock()

    async def _unload(self):
        pass  # A Decky reload must not disable an already configured game.

    async def _uninstall(self):
        await asyncio.to_thread(self.runtime.disable_all)

    async def get_games(self):
        return await asyncio.to_thread(self.runtime.games)

    async def prepare_game(self, appid: int, options: str):
        async with self.lock:
            return await asyncio.to_thread(self.runtime.prepare, appid, options)

    async def confirm_game(self, appid: int, actual: str):
        async with self.lock:
            return await asyncio.to_thread(self.runtime.confirm, appid, actual)

    async def disable_game(self, appid: int, actual: str):
        async with self.lock:
            return await asyncio.to_thread(self.runtime.disable, appid, actual)

    async def finish_restore(self, appid: int, actual: str):
        async with self.lock:
            await asyncio.to_thread(self.runtime.forget, appid, actual)

    async def get_status(self, appid: int):
        return await asyncio.to_thread(self.runtime.status, appid)

    async def export_report(self, appid: int):
        return await asyncio.to_thread(self.runtime.export, appid)

import asyncio
from pathlib import Path
import decky
from gsf_runtime import Runtime


class Plugin:
    async def _main(self):
        self.runtime = Runtime(decky.DECKY_USER_HOME, Path(__file__).resolve().parent)
        self.lock = asyncio.Lock()
        # Updating Decky must also update the installed launcher/build even if
        # Steam's AppDetails.readOptions API is broken or the game was already
        # configured. Otherwise current.json silently points to old GPU code.
        # Use the same verified, atomic installer as prepare_game; do not write
        # or synthesize Steam launch options from this startup path.
        await asyncio.to_thread(self.runtime.install)

    async def _unload(self):
        pass  # A Decky reload must not disable an already configured game.

    async def _uninstall(self):
        await asyncio.to_thread(self.runtime.disable_all)

    async def get_games(self):
        return await asyncio.to_thread(self.runtime.games)

    async def get_selection(self):
        return await asyncio.to_thread(self.runtime.selection)

    async def select_game(self, appid: int):
        async with self.lock:
            return await asyncio.to_thread(self.runtime.select, appid)

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

    async def set_diagnostic_mode(self, appid: int, mode: str):
        async with self.lock:
            return await asyncio.to_thread(self.runtime.set_diagnostic_mode, appid, mode)

    async def set_multiplier(self, appid: int, multiplier: int):
        async with self.lock:
            return await asyncio.to_thread(self.runtime.set_multiplier, appid, multiplier)

    async def export_report(self, appid: int):
        return await asyncio.to_thread(self.runtime.export, appid)

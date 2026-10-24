# SPDX-License-Identifier: GPL-3.0-or-later
"""RPC persistence, error rollback and migration; no SteamOS installation is performed."""
import asyncio
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "py_modules"))
from kedavra_ctl import Control
from kedavra_diagnostics import Diagnostics


class PluginRenderTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        spec = importlib.util.spec_from_file_location("kedavra_plugin_render_test", HERE.parent / "main.py")
        module = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {"decky": types.SimpleNamespace(logger=Mock()),
                                      "kedavra_hud": types.SimpleNamespace(Hud=Mock())}):
            spec.loader.exec_module(module)
        self.tmp = tempfile.TemporaryDirectory()
        self.plugin = module.Plugin()
        self.plugin.ctl = Mock(spec=Control)
        self.plugin.ctl.render_options.side_effect = Control.render_options
        self.plugin.ctl.runtime_options.side_effect = Control.runtime_options
        self.plugin.ctl.apply_render_options.side_effect = lambda settings, *args: Control.render_options(settings)
        self.plugin.ctl.apply_runtime_options.side_effect = lambda settings, *args: Control.runtime_options(settings)
        self.plugin.lock = asyncio.Lock()
        self.plugin.out = Path(self.tmp.name)
        self.plugin.settings_path = self.plugin.out / "settings.json"
        self.plugin.settings = dict(self.plugin.DEFAULTS)
        self.plugin.diagnostics = Diagnostics(self.plugin.out)

    async def asyncTearDown(self):
        self.tmp.cleanup()

    async def test_rpc_persists_on_and_off(self):
        result = await self.plugin.set_render_options("sharp", True)
        self.assertEqual(result, {"post_preset": "sharp", "post_direct_history": True})
        self.assertTrue(json.loads(self.plugin.settings_path.read_text())["post_direct_history"])
        result = await self.plugin.set_render_options("sharp", False)
        self.assertFalse(result["post_direct_history"])
        settings = await self.plugin.get_settings()
        self.assertFalse(settings["post_direct_history"])
        self.assertEqual(settings["post_preset"], "sharp")

    async def test_failed_rpc_preserves_settings_and_restores_previous_runtime_options(self):
        self.plugin.ctl.apply_render_options.side_effect = [RuntimeError("command failed"), {}]
        with self.assertRaises(RuntimeError):
            await self.plugin.set_render_options("sharp", True)
        self.assertFalse(self.plugin.settings["post_direct_history"])
        self.assertEqual(self.plugin.settings["post_preset"], "off")
        self.assertFalse(self.plugin.settings_path.exists())
        restored, strict = self.plugin.ctl.apply_render_options.call_args.args
        self.assertFalse(restored["post_direct_history"])
        self.assertFalse(strict)

    async def test_invalid_rpc_never_sends_commands(self):
        with self.assertRaises(ValueError):
            await self.plugin.set_render_options("unknown", True)
        self.plugin.ctl.apply_render_options.assert_not_called()

    async def test_runtime_options_persist_and_failure_logs_error_and_rollback(self):
        result = await self.plugin.set_runtime_options(True, True)
        self.assertEqual(result, {"direct_scan": True, "deadline_analysis": True})
        self.assertTrue(json.loads(self.plugin.settings_path.read_text())["deadline_analysis"])
        self.plugin.ctl.apply_runtime_options.side_effect = [RuntimeError("injected failure"), {}]
        with self.assertRaises(RuntimeError):
            await self.plugin.set_runtime_options(False, False)
        self.assertTrue(self.plugin.settings["direct_scan"])
        events = [json.loads(line) for line in self.plugin.diagnostics.path.read_text().splitlines()]
        self.assertTrue(any(e["event"] == "runtime_options_failed" and e["level"] == "error" for e in events))
        self.assertEqual(events[-1]["event"], "runtime_options_rollback")

    async def test_c1_failure_is_in_persistent_log(self):
        self.plugin.ctl.apply_render_options.side_effect = [RuntimeError("C1 command refused"), {}]
        with self.assertRaises(RuntimeError):
            await self.plugin.set_render_options("sharp", True)
        text = self.plugin.diagnostics.path.read_text()
        self.assertIn("C1 command refused", text)
        self.assertIn("c1_options_rollback", text)

    async def test_failed_settings_save_restores_previous_runtime_options(self):
        with patch.object(self.plugin, "_save_settings", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                await self.plugin.set_runtime_options(True, True)
        self.assertFalse(self.plugin.settings["direct_scan"])
        self.assertFalse(self.plugin.ctl.apply_runtime_options.call_args.args[0]["deadline_analysis"])

    async def test_settings_migration_and_invalid_experiment_default_off(self):
        self.plugin.settings_path.write_text(json.dumps({"hud": False}))
        settings = self.plugin._load_settings()
        self.assertFalse(settings["post_direct_history"])
        self.assertEqual(settings["post_preset"], "off")
        self.plugin.settings_path.write_text(json.dumps({"post_direct_history": "true", "post_preset": []}))
        settings = self.plugin._load_settings()
        self.assertFalse(settings["post_direct_history"])
        self.assertEqual(settings["post_preset"], "off")


if __name__ == "__main__":
    unittest.main()

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

PLUGIN = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLUGIN/'py_modules'))
from gsf_runtime import Runtime, app_id, atomic_json, launch_options, live_records

spec = importlib.util.spec_from_file_location('gsf_launch', PLUGIN/'launch.py')
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='gsf test spaces ')
        self.home = Path(self.tmp.name)
        self.runtime = Runtime(self.home, PLUGIN)

    def tearDown(self):
        self.tmp.cleanup()

    def test_backup_verified_enable_and_exact_restore(self):
        old = 'MANGOHUD=1 gamemoderun %command% --original'
        with patch.object(self.runtime, 'install', return_value={'build': 'test'}):
            prepared = self.runtime.prepare(42, old)
            self.assertIn('--appid 42 -- %command%', prepared['options'])
            self.assertTrue(prepared['options'].endswith('--original'))
            self.assertFalse(self.runtime.config()['games']['42']['enabled'])
            with self.assertRaises(ValueError):
                self.runtime.confirm(42, 'stale Steam value')
            self.runtime.confirm(42, prepared['options'])
            # An update must retain the original, not back up its own wrapper.
            again = self.runtime.prepare(42, prepared['options'])
            self.assertEqual(again['original'], old)
            restore = self.runtime.disable(42, prepared['options'])
            self.assertEqual(restore, {'options': old, 'restore': True})
            self.runtime.forget(42, old)
            self.assertNotIn('42', self.runtime.config()['games'])

    def test_diagnostic_selection_is_validated_and_persistent(self):
        with patch.object(self.runtime, 'install', return_value={'build': 'mock'}):
            self.runtime.prepare(42, '')
        with self.assertRaises(ValueError):
            self.runtime.set_diagnostic_mode(42, 'unknown-mode')
        self.runtime.set_diagnostic_mode(42, 'matched-compute')
        self.assertEqual(Runtime(self.home, PLUGIN).status(42)['diagnostic_mode'], 'matched-compute')
        self.runtime.set_diagnostic_mode(42, 'normal-no-shadow')
        self.assertEqual(self.runtime.status(42)['diagnostic_mode'], 'normal-no-shadow')
        self.runtime.set_multiplier(42, 3)
        self.assertEqual(self.runtime.status(42)['multiplier'], 3)
        self.assertEqual(Runtime(self.home, PLUGIN).status(42)['multiplier'], 3)
        with self.assertRaises(ValueError):
            self.runtime.set_diagnostic_mode(42, 'normal-adaptive-pairs')
        self.runtime.set_multiplier(42, 4)
        self.assertEqual(Runtime(self.home, PLUGIN).status(42)['multiplier'], 4)
        with self.assertRaises(ValueError):
            self.runtime.set_multiplier(42, 5)
        with self.assertRaises(ValueError):
            self.runtime.set_multiplier(42, True)
        self.runtime.set_multiplier(42, 2)
        self.runtime.set_diagnostic_mode(42, 'normal-adaptive-pairs')
        self.assertEqual(self.runtime.status(42)['diagnostic_mode'], 'normal-adaptive-pairs')
        self.runtime.set_diagnostic_mode(42, 'normal-queue-observe')
        self.assertEqual(self.runtime.status(42)['diagnostic_mode'], 'normal-queue-observe')
        self.runtime.set_diagnostic_mode(42, 'normal-no-watch')
        self.assertEqual(self.runtime.status(42)['diagnostic_mode'], 'normal-no-watch')

    def test_manual_profile_arms_without_steam_and_never_restores_unknown_options(self):
        with patch.object(self.runtime, 'install', return_value={'build': 'mock'}):
            prepared = self.runtime.prepare_manual(42)
        self.assertIn('--appid 42 -- %command%', prepared['options'])
        status = Runtime(self.home, PLUGIN).status(42)
        self.assertTrue(status['enabled'])
        self.assertTrue(status['configured'])
        self.assertTrue(status['manual_launch'])
        self.assertEqual(status['state'], 'waiting')
        self.runtime.set_multiplier(42, 4)
        self.runtime.set_buffer_depth(42, 2)
        self.assertEqual(self.runtime.status(42)['multiplier'], 4)
        restore = self.runtime.disable(42, prepared['options'])
        self.assertFalse(restore['restore'])
        self.assertEqual(restore['options'], prepared['options'])
        self.assertFalse(self.runtime.status(42)['enabled'])

    def test_manual_profile_keeps_known_backup_and_quality_settings(self):
        original = 'MANGOHUD=1 %command% --old'
        with patch.object(self.runtime, 'install', return_value={'build': 'mock'}):
            prepared = self.runtime.prepare(42, original)
            self.runtime.confirm(42, prepared['options'])
            self.runtime.set_multiplier(42, 3)
            manual = self.runtime.prepare_manual(42)
        self.assertEqual(manual['options'], prepared['options'])
        self.assertEqual(self.runtime.config()['games']['42']['original'], original)
        self.assertEqual(self.runtime.status(42)['multiplier'], 3)
        restore = self.runtime.disable(42, '')
        self.assertFalse(restore['restore'])
        self.assertEqual(self.runtime.config()['games']['42']['original'], original)

    def test_manual_profile_can_switch_to_verified_automatic_activation(self):
        with patch.object(self.runtime, 'install', return_value={'build': 'mock'}):
            self.runtime.prepare_manual(42)
            self.runtime.set_multiplier(42, 4)
            prepared = self.runtime.prepare(42, 'USER=1 %command%')
        self.runtime.confirm(42, prepared['options'])
        self.assertFalse(self.runtime.status(42)['manual_launch'])
        self.assertEqual(self.runtime.status(42)['multiplier'], 4)
        self.assertEqual(self.runtime.disable(42, prepared['options'])['options'], 'USER=1 %command%')

    def test_selection_persists_without_installing_or_enabling(self):
        self.assertEqual(self.runtime.selection(), {'appid': 0})
        self.runtime.select(42)
        reloaded = Runtime(self.home, PLUGIN)
        self.assertEqual(reloaded.selection(), {'appid': 42})
        self.assertEqual(reloaded.config()['games'], {})
        self.assertFalse((reloaded.root/'current.json').exists())

    def test_game_gate_refusal_is_visible_without_gpu_bridge(self):
        run = self.runtime.results/'game-42-20261018-test'
        run.mkdir(parents=True)
        (run/'game.log').write_text('GSF_REAL_WSI_GATE gpu_requested=1 candidate=0 reason=unsupported-swapchain-flags\n')
        result = self.runtime.status(42)
        self.assertEqual(result['state'], 'blocked')
        self.assertEqual(result['gate_rejection'], 'unsupported-swapchain-flags')
        with (run/'game.log').open('a') as f:
            f.write('GSF_REAL_WSI_GATE gpu_requested=1 candidate=1 reason=extended-usage-ready\n')
        self.assertEqual(self.runtime.status(42)['state'], 'waiting')

    def test_user_edits_never_overwritten_on_restore(self):
        with patch.object(self.runtime, 'install', return_value={}):
            p = self.runtime.prepare(42, '-windowed')
        self.runtime.confirm(42, p['options'])
        edited = p['options']+' --new-user-option'
        restore = self.runtime.disable(42, edited)
        self.assertFalse(restore['restore'])
        self.assertEqual(restore['options'], edited)
        self.assertFalse(self.runtime.config()['games']['42']['enabled'])

    def test_conflicting_generators_and_ambiguous_options_rejected(self):
        for old in ('gfg-launch %command%', 'mako %command%', '%command% %command%', '"%command%"'):
            with self.assertRaises(ValueError):
                launch_options(self.runtime.root, 42, old)
        for value in ('../42', 0, -1, True, 2**32):
            with self.assertRaises(ValueError):
                app_id(value)

    def test_disabled_and_uninstalled_wrapper_needs_no_payload(self):
        self.runtime.root.mkdir(parents=True)
        for source, target in ((PLUGIN/'launch.py', 'launch.py'),
                               (PLUGIN/'py_modules/gsf_runtime.py', 'gsf_runtime.py')):
            (self.runtime.root/target).write_bytes(source.read_bytes())
        command = [sys.executable, str(self.runtime.root/'launch.py'), '--appid', '42', '--',
                   sys.executable, '-c', 'import sys;print(repr(sys.argv[1:]));sys.exit(7)', 'two words', 'a; $x']
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 7)
        self.assertIn("['two words', 'a; $x']", result.stdout)
        atomic_json(self.runtime.root/'config.json', {'games': {'42': {'enabled': True}}})
        self.runtime.disable_all()
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 7)

    def test_game_environment_has_no_fixed_fps_or_gpu_readback(self):
        inherited = {'VK_LAYER_PATH': '/steam/explicit', 'VK_ADD_LAYER_PATH': '/other/explicit',
                     'VK_INSTANCE_LAYERS': 'VK_LAYER_VALVE_steam_overlay', 'GFG_NATIVE_OUTPUT_HZ': '10'}
        class Base:
            @staticmethod
            def prepare_gamescope_layer(run):
                return {'directory': run/'explicit-wsi', 'disable_environment': {'DISABLE_GAMESCOPE_WSI': '1'}}
        with patch.object(launch, 'candidate_module', return_value=Base):
            env = launch.game_environment(self.home/'kit', self.home/'run', inherited)
        self.assertEqual(env['GFG_NATIVE_COMPAT_BUFFER'], '1')
        self.assertEqual(env['GFG_NATIVE_COST_ADAPTIVE'], '1')
        self.assertEqual(env['GFG_NATIVE_MODE'], '2')
        with patch.object(launch, 'candidate_module', return_value=Base):
            three = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal', 3)
        self.assertEqual(three['GFG_NATIVE_MODE'], '3')
        self.assertEqual(three['GFG_NATIVE_PACING'], '1')
        self.assertNotIn('GFG_NATIVE_OUTPUT_HZ', three)
        with patch.object(launch, 'candidate_module', return_value=Base):
            with self.assertRaises(ValueError):
                launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal-adaptive-pairs', 3)
        self.assertEqual(env['GFG_NATIVE_SHADOW_ADMISSION'], '1')
        with patch.object(launch, 'candidate_module', return_value=Base):
            no_shadow = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal-no-shadow')
        self.assertEqual(no_shadow['GFG_NATIVE_SHADOW_ADMISSION'], '0')
        self.assertNotIn('GFG_NATIVE_DIAGNOSTIC_MODE', no_shadow)
        self.assertEqual(env['GFG_NATIVE_PACING'], '1')
        self.assertEqual(env['GFG_NATIVE_VISIBILITY_PROBE'], '1')
        self.assertEqual(env['GFG_NATIVE_LMC_TIER'], '2')
        self.assertEqual(env['GFG_NATIVE_TIMELINE_WATCH'], '1')
        self.assertEqual(env['GFG_NATIVE_CALIBRATED_TIMESTAMPS'], '1')
        with patch.object(launch, 'candidate_module', return_value=Base):
            adaptive = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal-adaptive-pairs')
        self.assertEqual(adaptive['GFG_NATIVE_ADAPTIVE_PAIRS'], '1')
        self.assertNotIn('GFG_NATIVE_DIAGNOSTIC_MODE', adaptive)

        self.assertEqual(env['GFG_NATIVE_LATENCY_ADMISSION'], '1')
        self.assertNotIn('GFG_NATIVE_QUEUE_OBSERVE', env)
        with patch.object(launch, 'candidate_module', return_value=Base):
            observed = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal-queue-observe')
        self.assertEqual(observed['GFG_NATIVE_QUEUE_OBSERVE'], '1')
        self.assertEqual({k:v for k,v in observed.items() if k != 'GFG_NATIVE_QUEUE_OBSERVE'}, env)
        self.assertNotIn('GFG_NATIVE_DIAGNOSTIC_MODE', observed)
        with patch.object(launch, 'candidate_module', return_value=Base):
            old_admission = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal-no-latency-admission')
        self.assertEqual(old_admission['GFG_NATIVE_LATENCY_ADMISSION'], '0')
        self.assertEqual(old_admission['GFG_NATIVE_CALIBRATED_TIMESTAMPS'], '1')
        self.assertNotIn('GFG_NATIVE_DIAGNOSTIC_MODE', old_admission)
        with patch.object(launch, 'candidate_module', return_value=Base):
            watch_off = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal-no-watch')
        self.assertEqual(watch_off['GFG_NATIVE_TIMELINE_WATCH'], '0')
        self.assertNotIn('GFG_NATIVE_DIAGNOSTIC_MODE', watch_off)
        with patch.object(launch, 'candidate_module', return_value=Base):
            matched = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'matched-compute')
        self.assertEqual(matched['GFG_NATIVE_DIAGNOSTIC_MODE'], 'matched-compute')
        with patch.object(launch, 'candidate_module', return_value=Base):
            no_cal = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal-no-calibration')
        self.assertEqual(no_cal['GFG_NATIVE_CALIBRATED_TIMESTAMPS'], '0')
        with patch.object(launch, 'candidate_module', return_value=Base):
            every = launch.game_environment(self.home/'kit', self.home/'run', inherited, 'normal-calibration-every-frame')
        self.assertEqual(every['GFG_NATIVE_CALIBRATION_EVERY_FRAMES'], '1')
        self.assertEqual(env['VK_INSTANCE_LAYERS'].split(':')[0], 'VK_LAYER_GFG_native')
        self.assertEqual(env['VK_INSTANCE_LAYERS'].split(':')[-1], 'VK_LAYER_FROG_gamescope_wsi_x86_64')
        self.assertIn('/steam/explicit', env['VK_LAYER_PATH'])
        self.assertIn('/other/explicit', env['VK_LAYER_PATH'])
        for key in ('GFG_NATIVE_OUTPUT_HZ', 'GFG_TEST_SOURCE_HZ', 'GFG_NATIVE_GPU_BRIDGE_VERIFY'):
            self.assertNotIn(key, env)

    def test_live_status_rejects_stale_or_reused_pid(self):
        run = self.home/'run'
        (run/'status').mkdir(parents=True)
        ticks = int(Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19])
        r = {'pid': os.getpid(), 'process_start_ticks': ticks, 'updated_unix_ms': time.time()*1000,
             'generated_wsi_accepted': 100}
        atomic_json(run/'status/live.json', r)
        self.assertEqual(len(live_records(run)), 1)
        r['process_start_ticks'] += 1
        atomic_json(run/'status/live.json', r)
        self.assertEqual(live_records(run), [])
        r['process_start_ticks'] = ticks
        r['updated_unix_ms'] -= 10000
        atomic_json(run/'status/live.json', r)
        self.assertEqual(live_records(run), [])


if __name__ == '__main__':
    unittest.main()

class MotionIntelligenceSettingsTests(RuntimeTests):
    def test_x4_buffer_quality_and_off_survive_reload(self):
        with patch.object(self.runtime, 'install', return_value={}):
            p = self.runtime.prepare(42, '%command%')
        self.runtime.confirm(42, p['options'])
        self.runtime.set_multiplier(42, 4)
        for depth in (0, 1, 2):
            self.runtime.set_buffer_depth(42, depth)
            for quality in ('ultra-lite', 'balanced', 'quality', 'adaptive'):
                self.runtime.set_reconstruction_quality(42, quality)
                status = Runtime(self.home, PLUGIN).status(42)
                self.assertEqual((status['multiplier'], status['buffer_depth'], status['reconstruction_quality']), (4, depth, quality))
        for bad in (-1, 3, True, 1.0):
            with self.assertRaises(ValueError):
                self.runtime.set_buffer_depth(42, bad)
        self.runtime.set_multiplier(42, 0)
        self.assertFalse(self.runtime.status(42)['enabled'])
        self.assertEqual(self.runtime.status(42)['multiplier'], 0)

"""Actual extracted Decky backend/launcher + real Vulkan/LMC. No Steam/scanout claim."""
import argparse
import asyncio
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import types
import zipfile


async def test(archive, output):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gsf extracted plugin spaces ') as temp:
        home = Path(temp)
        with zipfile.ZipFile(archive) as z:
            z.extractall(home/'plugins')
        plugin = home/'plugins/GSFTest'
        checksums = json.loads((plugin/'CHECKSUMS.json').read_text())
        for name, sha in checksums.items():
            assert hashlib.sha256((plugin/name).read_bytes()).hexdigest() == sha, name
        sys.path.insert(0, str(plugin/'py_modules'))
        sys.modules['decky'] = types.SimpleNamespace(DECKY_USER_HOME=str(home))
        spec = importlib.util.spec_from_file_location('extracted_gsf_plugin', plugin/'main.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        backend = module.Plugin()
        await backend._main()
        prepared = await backend.prepare_game(42, 'MANGOHUD=0 %command% --original')
        await backend.confirm_game(42, prepared['options'])
        current = json.loads((backend.runtime.root/'current.json').read_text())
        kit = Path(current['kit'])
        app = kit/'wsi-gpu-smoke'
        app.chmod(0o755)
        env = os.environ.copy()
        env.update(HOME=str(home), GFG_TEST_FRAMES='70', GFG_TEST_WIDTH='96', GFG_TEST_HEIGHT='64',
                   GFG_TEST_SOURCE_HZ='5', GFG_TEST_ABSOLUTE_PACING='1', GFG_TEST_EXPECT_GPU='1',
                   GFG_TEST_ALLOW_SKIPS='1', GFG_TEST_MUTABLE_SWAPCHAIN='1',
                   GFG_TEST_EXPECT_LAYER_PATH=str(kit/'libVkLayer_gfg_native.so'))
        launcher = [sys.executable, str(backend.runtime.root/'launch.py'), '--appid', '42', '--']
        launched = subprocess.run(launcher+[str(app)], env=env, capture_output=True, text=True, timeout=100)
        (output/'launcher.log').write_text(launched.stdout+launched.stderr)
        run = backend.runtime.latest(42)
        shutil.copytree(run, output/'x2-session', dirs_exist_ok=True)
        log = (run/'game.log').read_text()
        (output/'game.log').write_text(log)
        assert launched.returncode == 0, (launched.stdout+launched.stderr+log)[-7000:]
        assert (backend.runtime.root/'gsf_segments.py').is_file()
        assert 'GSF_MUTABLE_SWAPCHAIN unorm_srgb_alias_view=PASS' in log
        line = next(s for s in log.splitlines() if s.startswith('GFG native: gpu stats '))
        stats = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
        assert stats['real'] == 70 and stats['synthetic'] >= 60, stats
        assert stats['errors'] == stats['present_path_host_pixel_reads'] == stats['present_path_cpu_waits'] == 0, stats
        # Healthy calibrated generation needs no visibility override: those
        # probes are now recovery-only, not required every twelfth source.
        # Native bridge must emit real per-pair GPU timeline observations.
        timeline_events = [json.loads(line) for line in ''.join(p.read_text() for p in run.glob('frames*.jsonl')).splitlines()
                           if line.strip().startswith('{')]
        watched = [e for e in timeline_events if e.get('event') == 'timeline_watch']
        assert len(watched) >= 10, 'Timeline watcher did not observe generated GPU submissions'
        assert any(e.get('watch_end_reason') == 'threshold_reached' for e in watched), watched[:3]
        assert any(e.get('wait_threshold') < e.get('actual_signal_value') for e in watched), '2x timeline signal mapping incorrect'
        assert any(e.get('event') == 'timeline_watch_summary' for e in timeline_events), 'Missing watcher teardown summary'
        # Sequence numbers restart in every process/bridge. Verify the actual
        # packaged C producers and Python report agree on the full identity.
        def identity(e):
            return e['pid'], e['generation'], e['te_sequence']
        pairs = [e for e in timeline_events if e.get('event') == 'pair']
        assert pairs and all('te_submit_sequence' in e for e in pairs)
        assert all(e['te_submit_sequence'] in (0, e['te_sequence']) for e in pairs)
        origins = {identity(e) for e in pairs if e['te_submit_sequence'] and e['dispatched_outputs']}
        for e in timeline_events:
            if e.get('event') in ('gpu_pair', 'gpu_correlated'):
                assert identity(e) in origins, e
            if e.get('event') == 'timeline_watch' and e['submit_status'] == 'submitted':
                assert identity(e) in origins, e
        status = await backend.get_status(42)
        assert status['report']['result'] == 'GENERATION_OBSERVED', status
        assert status['state'] == 'waiting', 'retired GPU process must not be shown live'
        metadata = json.loads((run/'LAUNCH.json').read_text())['native_environment']
        assert metadata['GFG_NATIVE_MODE'] == '2' and metadata['GFG_NATIVE_PACING'] == '1'
        assert metadata['GFG_NATIVE_VISIBILITY_PROBE'] == '1'
        assert metadata['GFG_NATIVE_TIMELINE_WATCH'] == '1'
        assert metadata['GFG_NATIVE_LATENCY_ADMISSION'] == '1'
        assert all('submit_delay_budget_calibrated' in e for e in pairs)
        assert status['report']['diagnostic_mode'] == 'normal'
        assert status['report']['timeline_diagnostic']['available'] is True
        joins = status['report']['timeline_diagnostic']['gpu_sample_joins']
        for kind in ('gpu_pair', 'gpu_correlated'):
            assert joins[kind]['matched'] > 0, joins
            assert joins[kind]['unmatched'] == joins[kind]['ambiguous'] == 0, joins
        assert metadata['GFG_NATIVE_LMC_TIER'] == '2'
        assert 'GFG_NATIVE_OUTPUT_HZ' not in metadata and 'GFG_NATIVE_GPU_BRIDGE_VERIFY' not in metadata
        # Per-game multiplier must survive the extracted plugin's IPC and
        # remain independent from the inherited Steam/Proton environment.
        assert (await backend.set_multiplier(42, 3))['multiplier'] == 3
        assert (await backend.get_status(42))['multiplier'] == 3
        x3env = module.Plugin()
        await x3env._main()
        assert (await x3env.get_status(42))['multiplier'] == 3
        # Do not assert OLED scanout. Verify the *second* real-Vulkan launch
        # with x3 and both planned output slots by parsing its own telemetry.
        run_three = subprocess.run(launcher+[str(app)], env=env, capture_output=True, text=True, timeout=100)
        (output/'x3-launcher.log').write_text(run_three.stdout+run_three.stderr)
        assert run_three.returncode == 0, (run_three.stdout+run_three.stderr)[-5000:]
        run3 = backend.runtime.latest(42)
        shutil.copytree(run3, output/'x3-session', dirs_exist_ok=True)
        meta3 = json.loads((run3/'LAUNCH.json').read_text())['native_environment']
        assert meta3['GFG_NATIVE_MODE'] == '3', meta3
        events3 = [json.loads(x) for x in ''.join(p.read_text() for p in run3.glob('frames*.jsonl')).splitlines()
                   if x.lstrip().startswith('{')]
        frames3 = [x for x in events3 if x.get('event') == 'pair']
        assert frames3 and all(x['multiplier'] == 3 for x in frames3)
        assert any((x['planned_mask'] & 3) == 3 for x in frames3), 'x3 never planned two synthetic slots'
        assert any(x['dispatched_outputs'] == 2 for x in frames3), ('x3 GPU never dispatched two outputs', [(x['planned_mask'], x['dispatched_outputs'], x['admission_decision'], x['gpu_budget_ns']) for x in frames3][-20:])
        assert all(x['physical_displayed'] is None for x in frames3)
        assert (await backend.set_multiplier(42, 2))['multiplier'] == 2
        exported = await backend.export_report(42)
        with zipfile.ZipFile(exported['path']) as z:
            assert any(n.endswith('/REPORT.json') for n in z.namelist())
        shutil.copy2(exported['path'], output/'GSF-Game-Latest.zip')
        restore = await backend.disable_game(42, prepared['options'])
        assert restore == {'options': 'MANGOHUD=0 %command% --original', 'restore': True}
        await backend.finish_restore(42, restore['options'])
        # The launcher must survive plugin uninstall and even a missing payload.
        await backend._uninstall()
        (backend.runtime.root/'current.json').unlink()
        bypass = subprocess.run(launcher+[sys.executable, '-c', 'import sys;sys.exit(7)'], env=env)
        assert bypass.returncode == 7
        print('EXTRACTED_DECKY_PASS: actual backend, install, 70 real frames, >=60 synthetic, zero host pixel reads, restore, missing-payload bypass; Steam/OLED unverified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    asyncio.run(test(args.archive, args.output))

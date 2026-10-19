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
        # Reproduce the actual Deck failure: an updated plugin but stale
        # installed launcher/current.json. Startup must heal it without the
        # unavailable Steam AppDetails API or a user reinstall button.
        installed_root = home/'.local/share/gsf-test'
        installed_root.mkdir(parents=True, exist_ok=True)
        (installed_root/'current.json').write_text(json.dumps({
            'build': '0'*40, 'kit': '/nonexistent/old-gsf-build'}))
        backend = module.Plugin()
        await backend._main()
        startup_status = await backend.get_status(42)
        assert startup_status['build_matches'] is True, startup_status
        assert startup_status['packaged_source_commit'] == startup_status['active_build_source_commit']
        assert (installed_root/'launch.py').is_file(), 'startup did not repair installed launcher'
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
        # ECO45 deliberately omits the 2ms-polling timeline watcher (it is
        # started only for TE_ALGO_LMC_GPU). Keep that CPU overhead out of
        # the energy-oriented shared-flow mode. Instead require correlated
        # calibrated GPU-start observations and actual WSI acceptance; do not
        # weaken the >=60/70 synthetic acceptance gate above.
        timeline_events = [json.loads(line) for line in ''.join(p.read_text() for p in run.glob('frames*.jsonl')).splitlines()
                           if line.strip().startswith('{')]
        watched = [e for e in timeline_events if e.get('event') == 'timeline_watch']
        assert not watched, 'ECO45 unexpectedly started a polling watcher'
        correlated = [e for e in timeline_events if e.get('event') == 'gpu_correlated']
        gpu_pairs = [e for e in timeline_events if e.get('event') == 'gpu_pair']
        assert len(gpu_pairs) >= 10 and len(correlated) >= 10, (
            'No GPU-timestamp evidence for ECO45 generated work', len(gpu_pairs), len(correlated))
        assert sum(e.get('outputs', 0) for e in gpu_pairs) >= 10, 'No completed generated GPU outputs'
        assert any(e.get('calibration_status') == 'calibrated' for e in correlated), (
            'No calibrated GPU start observation')
        assert any(e.get('ready_ms', [0])[0] > 0 for e in gpu_pairs), (
            'No positive measured G1 completion time')
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
        assert metadata['GFG_NATIVE_ALGO'] == 'gsf' and metadata['GFG_NATIVE_FLOW_LITE'] == '1', metadata
        assert 'GFG_NATIVE_GSF_ECO' not in metadata, 'Flow Lite must not accidentally select ECO45'
        assert 'GFG_NATIVE_GPU_CLOCK' not in metadata, 'ECO45 must not force GPU clocks'
        assert (await backend.set_diagnostic_mode(42, 'normal-eco45'))['diagnostic_mode'] == 'normal-eco45'
        assert (await backend.set_diagnostic_mode(42, 'normal-lmc-gpu'))['diagnostic_mode'] == 'normal-lmc-gpu'
        assert (await backend.set_diagnostic_mode(42, 'normal'))['diagnostic_mode'] == 'normal'
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
        assert (await backend.set_multiplier(42, 4))['multiplier'] == 4
        assert (await backend.set_buffer_depth(42, 2))['buffer_depth'] == 2
        assert (await backend.set_reconstruction_quality(42, 'balanced'))['reconstruction_quality'] == 'balanced'
        four_env = dict(env, GFG_TEST_FRAMES='24')
        run_four = subprocess.run(launcher+[str(app)], env=four_env, capture_output=True, text=True, timeout=100)
        (output/'x4-launcher.log').write_text(run_four.stdout+run_four.stderr)
        assert run_four.returncode == 0, run_four.stdout+run_four.stderr
        run4 = backend.runtime.latest(42)
        shutil.copytree(run4, output/'x4-session', dirs_exist_ok=True)
        meta4 = json.loads((run4/'LAUNCH.json').read_text())['native_environment']
        assert meta4['GFG_NATIVE_MODE'] == '4' and meta4['GFG_NATIVE_BUFFER_DEPTH'] == '2'
        assert meta4['GFG_NATIVE_FLOW_LITE2'] == '1'
        events4 = [json.loads(x) for x in ''.join(p.read_text() for p in run4.glob('frames*.jsonl')).splitlines() if x.startswith('{')]
        pairs4 = [x for x in events4 if x.get('event') == 'pair']
        assert len(pairs4) == 24 and all(x['multiplier'] == 4 for x in pairs4)
        assert any(x['dispatched_outputs'] == 3 for x in pairs4), 'packaged x4 never dispatched G1/G2/G3'
        assert any(x.get('event') == 'synthetic_lifecycle' and x['slot_id'] == 2 and x['wsi_accepted_ns'] for x in events4), 'packaged G3 never accepted'
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
        print('EXTRACTED_DECKY_PASS: Flow Lite GPU pipeline, actual backend, install, 70 real frames, >=60 synthetic, zero host pixel reads, restore, missing-payload bypass; Steam/OLED unverified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    asyncio.run(test(args.archive, args.output))

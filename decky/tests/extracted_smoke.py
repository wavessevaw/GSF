"""Actual extracted Decky backend/launcher + real Vulkan/LMC. No Steam/scanout claim."""
import argparse
import asyncio
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
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
        log = (run/'game.log').read_text()
        (output/'game.log').write_text(log)
        assert launched.returncode == 0, log[-5000:]
        assert 'GSF_MUTABLE_SWAPCHAIN unorm_srgb_alias_view=PASS' in log
        line = next(s for s in log.splitlines() if s.startswith('GFG native: gpu stats '))
        stats = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
        assert stats['real'] == 70 and stats['synthetic'] >= 60, stats
        assert stats['errors'] == stats['present_path_host_pixel_reads'] == stats['present_path_cpu_waits'] == 0, stats
        assert '[gfg-native] visibility_probe frame=' in log, '0.1.4 probe path was never exercised'
        # Native bridge must emit real per-pair GPU timeline observations.
        timeline_events = [json.loads(line) for line in (run/'frames.jsonl').read_text().splitlines()
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
        assert status['report']['diagnostic_mode'] == 'normal'
        assert status['report']['timeline_diagnostic']['available'] is True
        joins = status['report']['timeline_diagnostic']['gpu_sample_joins']
        for kind in ('gpu_pair', 'gpu_correlated'):
            assert joins[kind]['matched'] > 0, joins
            assert joins[kind]['unmatched'] == joins[kind]['ambiguous'] == 0, joins
        assert metadata['GFG_NATIVE_LMC_TIER'] == '2'
        assert 'GFG_NATIVE_OUTPUT_HZ' not in metadata and 'GFG_NATIVE_GPU_BRIDGE_VERIFY' not in metadata
        exported = await backend.export_report(42)
        with zipfile.ZipFile(exported['path']) as z:
            assert any(n.endswith('/REPORT.json') for n in z.namelist())
        import shutil
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

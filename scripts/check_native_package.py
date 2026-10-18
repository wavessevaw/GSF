#!/usr/bin/env python3
"""Exercise the extracted Decky/native ZIPs in an isolated home, never Deck acceptance.

Run under a working software Vulkan/X display with the WSI smoke app built.
The backend is imported from the archive, not from the source checkout.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def extract(path, target):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        assert len(names) == len(set(names)), "duplicate archive member"
        assert archive.testzip() is None, "ZIP CRC failure"
        for item in archive.infolist():
            name = Path(item.filename)
            assert not name.is_absolute() and '..' not in name.parts
            assert '\\' not in item.filename
            if item.is_dir():
                continue
            destination = target / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archive.read(item))
            destination.chmod((item.external_attr >> 16) & 0o777)


CONTROLLER = '''
import asyncio,json,logging,sys,types
from pathlib import Path
root,home,mode=sys.argv[1:]
sys.path.insert(0,str(Path(root)/'py_modules'))
decky=types.ModuleType('decky')
decky.logger=logging.getLogger('gfg-extracted-package')
decky.DECKY_USER_HOME=home
sys.modules['decky']=decky
from gfg_plugin.plugin import Plugin
p=Plugin()
if mode=='startup':
    p.installation_service.current_package_host_compatibility=lambda: ('isolated',False,'legacy-not-under-test')
    p.configuration_service.enforce_unsupported_host_passthrough_if_needed=lambda: False
    p.flatpak_service.disable_incompatible_host_overrides=lambda: {'success':True}
    asyncio.run(p._main())
    result=p.governor_service.native_gsf_status()
    assert result['installed'] and result['launcher_ready'],result
    assert result['requested_mode']=='off',result
else:
    result=p.governor_service.set_native_gsf(mode)
    assert result['success'],result
print('GFG_RESULT='+json.dumps(result))
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', type=Path, required=True)
    parser.add_argument('--decky', type=Path, required=True)
    parser.add_argument('--wsi-app', type=Path, required=True)
    parser.add_argument('--validation-layers', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    extract(args.component, out / 'component')
    extract(args.decky, out / 'decky')
    component = out / 'component/GFG-Native-FG'
    plugin = out / 'decky/GFG-Extreme'
    checks = []
    for name in ('bin/gfg-diagnostics', 'bin/gfg-native/install.sh', 'bin/gfg-native/gfg-native.sh'):
        assert os.access(plugin / name, os.X_OK), name
    for line in (component / 'SHA256SUMS.txt').read_text().splitlines():
        expected, name = line.split('  ', 1)
        assert sha((component / name).read_bytes()) == expected, name
        assert (plugin / 'bin/gfg-native' / name).read_bytes() == (component / name).read_bytes(), name
    manifest = json.loads((component / 'MANIFEST.json').read_text())
    assert not manifest['source_worktree_dirty'] and manifest['source_commit']
    assert not manifest['hardware_acceptance'] and not manifest['physical_display_verified']
    package = json.loads((plugin / 'package.json').read_text())
    lock = json.loads((plugin / 'package-lock.json').read_text())
    assert package['version'] == lock['version'] == lock['packages']['']['version']
    for name, field in [('governor_service.py', 'VERSION'), ('log_report.py', 'CURRENT_VERSION')]:
        text = (plugin / 'py_modules/gfg_plugin' / name).read_text()
        assert re.search('^' + field + ' = "' + re.escape(package['version']) + '"', text, re.M)
    checks.append('archive-checksums-executable-modes-and-build-identity')
    home = out / 'home'; home.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith('GFG_NATIVE_')}
    env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'),
               XDG_DATA_HOME=str(home / '.local/share'), XDG_STATE_HOME=str(home / '.local/state'),
               VK_LAYER_PATH=str(args.validation_layers.resolve()),
               VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation', LP_NUM_THREADS='4',
               GFG_NATIVE_OUTPUT_HZ='9', GFG_TEST_SOURCE_HZ='3', GFG_TEST_VALIDATION='1',
               GFG_TEST_FRAMES='6', GFG_TEST_ALLOW_SKIPS='1')

    def run(name, command, extra=None, expected=()):
        result = subprocess.run([str(x) for x in command], env=dict(env, **(extra or {})),
                                capture_output=True, text=True, timeout=120)
        log = result.stdout + result.stderr
        (out / (name + '.log')).write_text(log)
        assert result.returncode == 0, (name, result.returncode, log[-2000:])
        assert not re.search(r'Validation Error|VALIDATION ERROR|VUID-', log), name
        assert all(text in log for text in expected), (name, expected, log[-1000:])
        checks.append(name)
        print(name, 'PASS', flush=True)
        return log

    def backend(mode):
        return run('backend-' + mode, [sys.executable, '-c', CONTROLLER, plugin, home, mode])

    backend('startup')
    launcher = home / '.local/bin/gfg-native'
    assert os.access(launcher, os.X_OK)
    library = home / '.local/share/gfg-native/libVkLayer_gfg_native.so'
    assert sha(library.read_bytes()) == manifest['elf_sha256']
    foreign_config = home / 'inherited-runtime-config'
    foreign_mode = foreign_config / 'mako-render/runtime-state/native-gsf.mode'
    foreign_mode.parent.mkdir(parents=True)
    foreign_mode.write_text('3\n')
    log = run('startup-off-actual-wsi', [launcher, args.wsi_app],
              {'XDG_CONFIG_HOME': str(foreign_config)}, expected=['validation errors=0'])
    assert 'GPU bridge active' not in log, 'Decky Off was overridden by inherited XDG_CONFIG_HOME'
    for mode in ('2', '3'):
        backend(mode)
        extra = {'GFG_TEST_EXPECT_GPU': '1', 'GFG_TEST_CHAIN_VK12': '1',
                 'GFG_TEST_READONLY_FEATURES': '1'}
        if mode == '3':
            extra['GFG_TEST_PROTON_CHAIN'] = '1'
        log = run('installed-x' + mode, [launcher, '--diagnose', args.wsi_app],
                  extra, ['validation errors=0', 'GPU bridge WSI smoke PASS'])
        assert 'GPU bridge active' in log
        if mode == '3':
            assert 'original_metadata_unchanged=PASS' in log
    run('explicit-off-inherited-layer-actual-wsi', [launcher, '--off', args.wsi_app],
        {'VK_INSTANCE_LAYERS': 'VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation',
         'VK_LOADER_LAYERS_ENABLE': 'VK_LAYER_GFG_native', 'GFG_NATIVE_FG': '1',
         'GFG_NATIVE_GPU_BRIDGE': '1'}, ['validation errors=0'])
    blocked = home / 'not-a-state-directory'; blocked.write_text('fixture')
    run('unavailable-diagnostics-actual-wsi', [launcher, '--off', '--diagnose', args.wsi_app],
        {'XDG_STATE_HOME': str(blocked)}, ['without capture', 'validation errors=0'])
    library.rename(library.with_suffix('.disabled'))
    try:
        run('missing-component-actual-wsi', [launcher, args.wsi_app], expected=['validation errors=0'])
    finally:
        library.with_suffix('.disabled').rename(library)
    captures = list((home / '.local/state/gfg-native/diagnostics').glob('*/report.md'))
    assert len(captures) == 2
    observed_modes = set()
    for report in captures:
        assert 'NOT ACCEPTANCE' in report.read_text()
        assert (report.parent / 'pairs.jsonl').stat().st_size > 0
        pairs = [json.loads(line) for line in (report.parent / 'pairs.jsonl').read_text().splitlines() if line.strip()]
        pairs = [p for p in pairs if p.get('event') == 'pair']
        assert pairs, 'no real installed-layer pair telemetry'
        modes = {p['multiplier'] for p in pairs}
        assert len(modes) == 1, modes
        observed_modes.update(modes)
        if modes == {3}:
            ordered = [p for p in pairs if p.get('deferred_g2') and p['wsi_accepted_ns'][0] and p.get('g2_submit_return_ns')]
            assert ordered, 'installed x3 never handed off G1 before submitting G2'
            assert all(p['g2_submit_return_ns'] >= p['wsi_accepted_ns'][0] for p in ordered)
        else:
            assert modes == {2} and not any(p.get('deferred_g2') for p in pairs)
    assert observed_modes == {2, 3}, observed_modes
    checks.append('installed-x3-g1-before-g2-submission-and-x2-unchanged')
    assert not list((home / '.local/state/gfg-native/status').glob('*.json'))
    checks.append('two-local-pair-reports-and-retired-status-cleanup')
    run('first-install-rollback', ['bash', component / 'rollback.sh'])
    assert not library.exists()
    run('rollback-off-actual-wsi', [launcher, '--off', args.wsi_app], expected=['validation errors=0'])
    result = dict(scope='isolated extracted-package software Vulkan; NOT Steam Deck acceptance',
                  source_commit=manifest['source_commit'], version=package['version'],
                  component_sha256=sha(args.component.read_bytes()), decky_sha256=sha(args.decky.read_bytes()),
                  elf_sha256=manifest['elf_sha256'], checks=checks, passed=True,
                  hardware_acceptance='NOT TESTED', physical_displayed_fps=None)
    (out / 'acceptance.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()

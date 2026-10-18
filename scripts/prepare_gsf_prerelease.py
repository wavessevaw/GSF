#!/usr/bin/env python3
"""Stage verified, immutable Native GSF prerelease assets; never rebuild a ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--ci-evidence', type=Path)
    args = parser.parse_args()
    spec = json.loads((ROOT / 'docs/fg3/evidence/prerelease-internal_10.json').read_text())
    version = spec['version']
    source = spec['source_commit']
    assert version == '2.1.5-gsf.10' and spec['tag'] == 'v' + version
    artifact_dir = ROOT / 'docs/fg3/artifacts'
    decky = artifact_dir / ('GFG-Extreme-' + version + '.zip')
    component = artifact_dir / 'GFG-Native-FG-v0_0_0-fg3-internal_10.zip'
    assert sha(decky) == spec['decky_sha256']
    assert sha(component) == spec['component_sha256']
    assert spec['passed'] and len(spec['checks']) == 14
    assert spec['physical_displayed_fps'] is None
    for path in [decky, component]:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            assert len(names) == len(set(names)) and archive.testzip() is None
            assert all(not Path(n).is_absolute() and '..' not in Path(n).parts and '\\' not in n for n in names)
    with zipfile.ZipFile(component) as native, zipfile.ZipFile(decky) as integrated:
        manifest = json.loads(native.read('GFG-Native-FG/MANIFEST.json'))
        assert manifest['source_commit'] == source and manifest['source_worktree_dirty'] is False
        assert manifest['elf_sha256'] == spec['elf_sha256']
        assert manifest['hardware_acceptance'] is False and manifest['physical_display_verified'] is False
        metadata = json.loads(integrated.read('GFG-Extreme/package.json'))
        assert metadata['version'] == version and metadata['gfgPrerelease'] is True
        for line in native.read('GFG-Native-FG/SHA256SUMS.txt').decode().splitlines():
            expected, name = line.split('  ', 1)
            data = native.read('GFG-Native-FG/' + name)
            assert hashlib.sha256(data).hexdigest() == expected, name
            assert integrated.read('GFG-Extreme/bin/gfg-native/' + name) == data, name
            if name.startswith('SOURCE/'):
                original = subprocess.check_output(['git', 'show', source + ':' + name.removeprefix('SOURCE/')], cwd=ROOT)
                assert original == data, name
        elf = native.read('GFG-Native-FG/libVkLayer_gfg_native.so')
        assert hashlib.sha256(elf).hexdigest() == spec['elf_sha256']
    # Only release infrastructure/docs/tests may change after the package source.
    subprocess.run(['git', 'diff', '--exit-code', source, 'HEAD', '--', 'engine', 'py_modules',
                    'frontend', 'dist', 'package.json', 'package-lock.json', 'plugin.json',
                    'scripts/package_native_fg.py', 'scripts/accept_report.py'], cwd=ROOT, check=True)
    args.out.mkdir(parents=True, exist_ok=False)
    for path in [decky, component, ROOT / 'docs/fg3/evidence/prerelease-internal_10.json',
                 ROOT / 'docs/fg3/evidence/prerelease-internal_10.zip']:
        shutil.copyfile(path, args.out / path.name)
    shutil.copyfile(ROOT / 'docs/fg3/PRERELEASE_2_1_5_GSF_10.ru.md', args.out / 'RELEASE_NOTES.ru.md')
    if args.ci_evidence:
        reports = [('pipeline/acceptance.json', 17), ('hdr/acceptance.json', 6), ('package/acceptance.json', 14)]
        for name, count in reports:
            result = json.loads((args.ci_evidence / name).read_text())
            assert result['passed'] and len(result['checks']) == count, name
            if name.startswith('package/'):
                assert result['decky_sha256'] == spec['decky_sha256'] and result['elf_sha256'] == spec['elf_sha256']
        with zipfile.ZipFile(args.out / ('CI-VALIDATION-' + spec['tag'] + '.zip'), 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(args.ci_evidence.rglob('*')):
                # Exclude extracted homes, plugin payloads and capture system metadata.
                name = path.relative_to(args.ci_evidence)
                if path.is_file() and len(name.parts) <= 2 and path.suffix in {'.log', '.json', '.jsonl'}:
                    archive.write(path, name.as_posix())
    (args.out / 'SHA256SUMS.txt').write_text(''.join(
        sha(path) + '  ' + path.name + '\n' for path in sorted(args.out.iterdir()) if path.is_file()))
    print('Verified prerelease assets:', spec['tag'], 'source=' + source)


if __name__ == '__main__':
    main()

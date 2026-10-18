#!/usr/bin/env python3
"""Build an offline Decky ZIP from the same verified GPU candidate payload."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--candidate', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    args = ap.parse_args()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=ROOT, text=True):
        raise ValueError('Refuse a dirty source build')
    with tempfile.TemporaryDirectory(prefix='gsf-decky-package-') as temporary:
        plugin = Path(temporary)/'GSFTest'
        plugin.mkdir()
        for name in ('main.py', 'launch.py', 'plugin.json', 'package.json', 'README.md'):
            shutil.copy2(ROOT/'decky'/name, plugin/name)
        shutil.copytree(ROOT/'decky/py_modules', plugin/'py_modules', ignore=shutil.ignore_patterns('__pycache__'))
        (plugin/'dist').mkdir()
        shutil.copy2(ROOT/'decky/dist/index.js', plugin/'dist/index.js')
        for name in ('LICENSE.md', 'NOTICE.md', 'COPYRIGHT.md'):
            shutil.copy2(ROOT/name, plugin/name)
        # Include sources/licenses of the Decky API glue and its rollup loader
        # shim. UI and React remain provided by the user's Decky installation.
        for package in ('api', 'rollup'):
            source = ROOT/'decky/node_modules/@decky'/package
            dest = plugin/'third_party'/('decky-'+package)
            dest.mkdir(parents=True)
            for name in ('LICENSE', 'package.json'):
                shutil.copy2(source/name, dest/name)
            shutil.copytree(source/'src', dest/'src')
        with zipfile.ZipFile(args.candidate) as candidate:
            prefix = 'GSF-Deck-Test/'
            checksums = json.loads(candidate.read(prefix+'CHECKSUMS.json'))
            info = json.loads(candidate.read(prefix+'BUILD_INFO.json'))
            if info['source_commit'] != commit:
                raise ValueError('GPU candidate and plugin source commits differ')
            for name in (*checksums, 'CHECKSUMS.json'):
                relative = Path(name)
                if relative.is_absolute() or '..' in relative.parts:
                    raise ValueError('Unsafe payload path')
                data = candidate.read(prefix+name)
                if name in checksums and hashlib.sha256(data).hexdigest() != checksums[name]:
                    raise ValueError('Payload checksum mismatch: '+name)
                path = plugin/'payload'/relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        with (plugin/'SOURCE.tar.gz').open('wb') as source:
            subprocess.run(['git', 'archive', '--format=tar.gz', 'HEAD', 'decky', 'engine/gfg-native',
                            'engine/gfg-temporal', 'scripts/package_decky_test.py',
                            'scripts/package_deck_candidate.py', 'LICENSE.md', 'NOTICE.md', 'COPYRIGHT.md'],
                           cwd=ROOT, stdout=source, check=True)
        hashes = {p.relative_to(plugin).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in plugin.rglob('*') if p.is_file()}
        (plugin/'CHECKSUMS.json').write_text(json.dumps(hashes, indent=2)+'\n')
        args.out.mkdir(parents=True, exist_ok=True)
        archive = args.out/'GSF-Decky-Test-0.1.0.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for p in sorted(plugin.rglob('*')):
                if p.is_file():
                    z.write(p, 'GSFTest/'+p.relative_to(plugin).as_posix())
        print(archive)


if __name__ == '__main__':
    main()

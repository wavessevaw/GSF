#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline GSF Next package, complete corresponding sources and checksums."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[1]
UPSTREAM='bc56e5bd0e3a6c328aedef664d0d36363fb58b36'


def dump(path,obj):path.write_text(json.dumps(obj,indent=2,ensure_ascii=False)+'\n')


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--build',type=Path,default=ROOT/'engine/gsf-next/build');ap.add_argument('--out',type=Path,default=ROOT/'artifacts/gsf-next-release');a=ap.parse_args()
    sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    if subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=ROOT,text=True):raise ValueError('Commit source changes before packaging')
    info=json.loads((a.build/'BUILD_INFO.json').read_text())
    if info['source_commit']!=sha:raise ValueError('Rebuild native layer from the packaging commit')
    version=json.loads((ROOT/'decky-next/package.json').read_text())['version']
    with tempfile.TemporaryDirectory(prefix='gsf-next-package-') as tmp:
        plugin=Path(tmp)/'GSFNext';plugin.mkdir()
        for name in ('main.py','launch.py','plugin.json','package.json','README.md'):shutil.copy2(ROOT/'decky-next'/name,plugin/name)
        shutil.copytree(ROOT/'decky-next/py_modules',plugin/'py_modules',ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
        shutil.copytree(ROOT/'decky-next/dist',plugin/'dist')
        shutil.copytree(ROOT/'docs/gsf-next',plugin/'docs')
        shutil.copy2(ROOT/'LICENSE.md',plugin/'LICENSE.md')
        notice=plugin/'third_party';notice.mkdir()
        shutil.copy2(ROOT/'engine/gsf-next/LICENSE',notice/'AFMF-Linux-LICENSE.txt')
        shutil.copy2(ROOT/'engine/gsf-next/UPSTREAM.md',notice/'AFMF-Linux-UPSTREAM.md')
        shutil.copy2(ROOT/'engine/gsf-next/shaders/fidelityfx/LICENSE.txt',notice/'FidelityFX-LICENSE.txt')
        shutil.copy2(ROOT/'engine/gsf-next/shaders/fidelityfx/NOTICE.md',notice/'FidelityFX-NOTICE.md')
        for package in ('api','rollup'):
            source=ROOT/'decky-next/node_modules/@decky'/package;dest=notice/('decky-'+package);dest.mkdir()
            for name in ('LICENSE','package.json'):shutil.copy2(source/name,dest/name)
            shutil.copytree(source/'src',dest/'src')
        payload=plugin/'payload';payload.mkdir()
        shutil.copy2(a.build/'layer/libafmf-linux.so',payload/'libgsf-next.so')
        dump(payload/'BUILD_INFO.json',info)
        dump(payload/'gsf-next.json',dict(file_format_version='1.2.1',layer=dict(name='VK_LAYER_GSF_next',type='GLOBAL',library_path='./libgsf-next.so',library_arch='64',api_version='1.3.0',implementation_version='1',description='GSF Next: modified AFMF-Linux / FidelityFX Optical Flow',functions=dict(vkNegotiateLoaderLayerInterfaceVersion='vkNegotiateLoaderLayerInterfaceVersion'))))
        dump(payload/'CHECKSUMS.json',{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in payload.iterdir()})
        with (plugin/'SOURCE.tar.gz').open('wb') as source:
            subprocess.run(['git','archive','--format=tar.gz','HEAD','engine/gsf-next','decky-next','scripts/package_gsf_next.py','scripts/build_gsf_next.sh','scripts/report_gsf_next.py','.github/workflows/gsf-next.yml','docs/gsf-next','LICENSE.md'],cwd=ROOT,stdout=source,check=True)
        dump(plugin/'CHECKSUMS.json',{p.relative_to(plugin).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in plugin.rglob('*') if p.is_file()})
        a.out.mkdir(parents=True,exist_ok=True);archive=a.out/('GSF-Next-'+version+'.zip')
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
            for p in sorted(plugin.rglob('*')):
                if p.is_file():z.write(p,'GSFNext/'+p.relative_to(plugin).as_posix())
        checksum=hashlib.sha256(archive.read_bytes()).hexdigest()
        (a.out/(archive.name+'.sha256')).write_text(checksum+'  '+archive.name+'\n')
        print(archive);print(checksum)

if __name__=='__main__':main()

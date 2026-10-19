#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the actual installable ZIP, then load its real Decky backend and launcher."""
import asyncio
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import zipfile


def main():
    archive=Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='gsf-next-extracted-') as temporary:
        root=Path(temporary);home=root/'home';home.mkdir()
        with zipfile.ZipFile(archive) as z:
            for name in z.namelist():
                assert name.startswith('GSFNext/') and '..' not in Path(name).parts
            z.extractall(root)
        plugin=root/'GSFNext'
        checks=json.loads((plugin/'CHECKSUMS.json').read_text())
        for name,expected in checks.items():
            assert hashlib.sha256((plugin/name).read_bytes()).hexdigest()==expected,name
        assert (plugin/'SOURCE.tar.gz').stat().st_size>10000
        assert (plugin/'third_party/AFMF-Linux-LICENSE.txt').exists()
        sys.path.insert(0,str(plugin/'py_modules'))
        sys.modules['decky']=types.SimpleNamespace(DECKY_USER_HOME=str(home))
        spec=importlib.util.spec_from_file_location('gsf_next_extracted_backend',plugin/'main.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        backend=module.Plugin()
        async def exercise():
            await backend._main()
            for mult in (2,3,4):
                await backend.prepare_game(1)
                await backend.configure_game(1,dict(multiplier=mult))
                status=await backend.get_status(1)
                assert status['profile']['enabled'] and status['profile']['multiplier']==mult
            await backend._uninstall()
            assert not (await backend.get_status(1))['profile']['enabled']
        asyncio.run(exercise())
        launcher=home/'.local/share/gsf-next/launch.py'
        child='import os; assert os.environ.get("GSF_NEXT_MULTIPLIER") is None; print("off-pass-through")'
        env=os.environ.copy();env['HOME']=str(home)
        result=subprocess.run([sys.executable,str(launcher),'--appid','1','--',sys.executable,'-c',child],env=env,capture_output=True,text=True,timeout=10)
        assert result.returncode==0 and 'off-pass-through' in result.stdout,(result.returncode,result.stderr)
        # Optional actual Vulkan harness when run in the Vulkan build environment.
        harness=os.environ.get('GSF_NEXT_SMOKE_HARNESS')
        if harness:
            for mult in (2,3,4):
                asyncio.run(backend.prepare_game(1));asyncio.run(backend.configure_game(1,dict(multiplier=mult)))
                # The source harness enables this exact layer name and validation.
                # Keep production zero-timeout/acquire, quality and pacing defaults.
                env.update(AFMF_TEST_X11='1',AFMF_TEST_FRAMES='16',AFMF_TEST_FRAME_MS='30')
                result=subprocess.run(['xvfb-run','-a',sys.executable,str(launcher),'--appid','1','--',harness],env=env,capture_output=True,text=True,timeout=90)
                assert result.returncode==0,(mult,result.returncode,result.stderr)
                run=backend.runtime.latest(1);log=(run/'game.log').read_text()
                assert 'generation on' in log and 'GSF_NEXT_SUMMARY' in log,log[-2000:]
                assert 'validation:' not in log and 'VUID-' not in log,log[-3000:]
                import re
                totals=re.findall(r'GSF_NEXT_SUMMARY real=(\d+) built=(\d+) accepted_real=(\d+) accepted_synthetic=(\d+)',log)
                assert totals and int(totals[-1][1])>0 and int(totals[-1][2])==16 and totals[-1][1]==totals[-1][3],totals
                print('extracted launcher Vulkan ×'+str(mult)+': PASS',flush=True)
    print('extracted ZIP: checksums, offline install, actual backend, profiles and off launch PASS')

if __name__=='__main__':main()

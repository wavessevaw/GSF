#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""No-layer pass-through when disabled; preflight failures never stop the game."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time
from next_runtime import Runtime, app_id, atomic, environment, read, verify


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--appid',required=True)
    parser.add_argument('command',nargs=argparse.REMAINDER)
    args=parser.parse_args()
    command=args.command[1:] if args.command[:1]==['--'] else args.command
    if not command: parser.error('missing game command after --')
    aid=app_id(args.appid)
    root=Path(__file__).resolve().parent
    inherited=os.environ.copy()
    try: profile=read(root/'config.json',{}).get('games',{}).get(aid,{})
    except (OSError,ValueError,TypeError): profile={}
    if not profile.get('enabled') or profile.get('multiplier',2)==0:
        os.execvpe(command[0],command,inherited)
    runtime=Runtime(Path.home(),root)
    runtime.root=root
    runtime.results.mkdir(parents=True,exist_ok=True,mode=0o700)
    run=Path(tempfile.mkdtemp(prefix='game-'+aid+'-'+str(time.time_ns())+'-',dir=runtime.results))
    (run/'status').mkdir(mode=0o700)
    report=dict(schema=1,appid=int(aid),stage='preflight',displayed=None)
    try:
        if platform.machine()!='x86_64': raise ValueError('Нужна Linux x86_64')
        current=read(root/'current.json')
        kit=Path(current['kit'])
        report['build']=verify(kit)
        env=environment(kit,run,inherited,profile)
        report['profile']=profile
        report['result']='NO_GENERATION_EVIDENCE'
    except (OSError,ValueError,KeyError,TypeError) as error:
        # No automatic retry after starting the game: only preflight may fail open.
        env=inherited
        report.update(result='REAL_ONLY_PREFLIGHT_FAILURE',error=str(error))
        print('GSF Next: обычный запуск: '+str(error),file=sys.stderr)
    report['stage']='running'
    atomic(run/'REPORT.json',report)
    sampler=None
    try:
        from gsf_amdgpu import GpuSampler
        sampler=GpuSampler(run/'amdgpu.jsonl')
        sampler.start()
    except (ImportError,OSError): pass
    try:
        with (run/'game.log').open('ab',buffering=0) as output:
            child=subprocess.Popen(command,env=env,stdout=output,stderr=subprocess.STDOUT)
            while child.poll() is None:
                # Bound game logs without pipe readers or blocking the render process.
                if (run/'game.log').stat().st_size>16*1024*1024:
                    with (run/'game.log').open('r+b') as log: log.truncate(0)
                    report['log_truncated']=True
                time.sleep(.5)
        code=child.returncode
        report.update(stage='finished',exit_code=code)
    except OSError as error:
        code=127
        report.update(stage='failed',error=str(error),exit_code=code)
    finally:
        if sampler: sampler.stop()
        atomic(run/'REPORT.json',report)
    return code if code>=0 else 128-code


if __name__=='__main__': sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual Vulkan API spacing, late-ready output, and in-process control changes.
Not a physical presentation or hardware performance benchmark.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def run(build, out, name, patch, frames=24):
    directory=out/name
    if directory.exists(): shutil.rmtree(directory)
    directory.mkdir(parents=True)
    env=os.environ.copy()
    env.update(VK_LAYER_PATH=str(build/'layer')+':/usr/share/vulkan/explicit_layer.d',
               VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING='1',VK_LOADER_LAYERS_DISABLE='~implicit~',
               AFMF_TEST_X11='1',AFMF_LOG='3',AFMF_TEST_FRAMES=str(frames),AFMF_TEST_FRAME_MS='21',
               AFMF_MIN_FPS='0',AFMF_GOVERNOR='0',AFMF_EXTRA_IMAGES='3',AFMF_PERFORMANCE_MODE='performance',
               AFMF_SEARCH_MODE='standard',AFMF_ACQUIRE_TIMEOUT_US='16000',
               GSF_NEXT_REFRESH_HZ='90',GSF_NEXT_MULTIPLIER='3',GSF_NEXT_DIAGNOSTICS='1',
               GSF_NEXT_STATUS_DIR=str(directory),GSF_NEXT_REAL_PRIORITY_PACING='0',GSF_NEXT_PRESENT_REPORT=str(directory/'present'),
               GSF_NEXT_GPU_REPORT=str(directory/'gpu'))
    if name in ('live','live-diag'):
        path=directory/'control';path.write_text('GSF_NEXT_CONTROL_V1\n1 1 2 90 2 2 3 0 1\n')
        env['GSF_NEXT_CONTROL']=str(path)
    env.update(patch)
    result=subprocess.run(['xvfb-run','-a',str(build/'afmf_headless')],env=env,
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
    (directory/'run.log').write_text(result.stdout)
    assert result.returncode==0,(name,result.returncode,result.stdout[-5000:])
    assert 'VUID-' not in result.stdout and 'validation:' not in result.stdout,(name,result.stdout[-5000:])
    totals=re.findall(r'GSF_NEXT_SUMMARY real=(\d+) built=(\d+) accepted_real=(\d+) accepted_synthetic=(\d+)',result.stdout)
    assert sum(int(r[2]) for r in totals)==frames,(name,totals)
    if name=='deadline-priority':
        assert all(int(r[1])>=int(r[3]) for r in totals),(name,'constructed/accepted inverted',totals)
    else:
        assert all(r[1]==r[3] for r in totals),(name,'lost constructed output',totals)
    if name=='live-diag':
        assert 'GPU timing detail changed: compact' in result.stdout,'compact timing never applied'
        assert 'GPU timing detail changed: detailed' in result.stdout,'detailed timing never restored'
        summary=dict(case=name,real=frames,synthetic=sum(int(r[3]) for r in totals),displayed=None)
        print(json.dumps(summary),flush=True);return summary
    records=[]
    for path in directory.glob('present.*.jsonl'):
        records.extend(json.loads(line) for line in path.read_text().splitlines())
    records.sort(key=lambda r:r['submit_ns'])
    assert len(records)==sum(int(r[2])+int(r[3]) for r in totals),(name,'trace missing API calls')
    if name!='deadline-priority':
        assert any(not r['real'] for r in records),(name,'no synthetic')
    for before,after in zip(records,records[1:]):
        if after['enabled']:
            tick=(1_000_000_000+after['refresh_hz']-1)//after['refresh_hz']
            assert after['submit_ns']-before['submit_ns']>=tick//2,(name,'compressed API output',before,after)
            if before['refresh_hz']==after['refresh_hz']:
                assert after['target_ns']-before['target_ns']>=tick,(name,'reused target slot',before,after)
    pairs={}
    for row in records:
        assert row['result'] in (0,1000001003),(name,row)
        assert row['submit_ns']>=row['ready_observed_ns'],(name,'present before ready',row)
        pairs.setdefault(row['source_b'],[]).append(row)
    for pair in pairs.values():
        assert pair[-1]['real'] and all(not r['real'] for r in pair[:-1]),(name,'real/synthetic order',pair)
        assert [r['t'] for r in pair]==sorted(set(r['t'] for r in pair)),(name,'phase order',pair)
        if pair[0]['enabled']:
            assert len(pair)<=pair[0]['multiplier'],(name,'extra output',pair)
    if name=='deadline-priority':
        snapshots=[json.loads(path.read_text()) for path in directory.glob('*.json')]
        skipped=max((int(s.get('expired_synthetic',0))+int(s.get('superseded_synthetic',0))
                     for s in snapshots),default=0)
        assert skipped>0,(name,'no expired frames under injected late readiness',snapshots)
        assert len([r for r in records if r['real']])==frames,(name,'lost real frames')
    if name=='live':
        revisions={r['revision'] for r in records}
        assert revisions==set(range(1,9)),('missing applied revisions',revisions)
        for rev,mult in enumerate((2,3,4,0,2,4,3,4),1):
            rows=[r for r in records if r['revision']==rev]
            assert all(r['multiplier']==mult for r in rows),(rev,rows)
            if rev==4: assert all(r['real'] and not r['enabled'] for r in rows),'Off kept generating'
        assert any(r['revision']==5 and r['quality']==1 for r in records),'quality not hot-applied'
        assert any(r['revision']==6 and r['quality']==2 and r['queue_limit']==1 for r in records),'quality/queue not restored'
        assert 'live swapchain recreation' in result.stdout,'WSI settings never recreated'
        assert any(r['revision']==8 and r['refresh_hz']==60 for r in records),'refresh not hot-applied'
    summary=dict(case=name,real=frames,synthetic=sum(not r['real'] for r in records),
        minimum_enabled_api_gap_ms=min((b['submit_ns']-a['submit_ns'])/1e6 for a,b in zip(records,records[1:]) if b['enabled']),
        displayed=None)
    print(json.dumps(summary),flush=True)
    return summary


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build',type=Path,required=True);parser.add_argument('--out',type=Path)
    args=parser.parse_args();build=args.build.resolve();out=(args.out or build/'Testing/live-pacing').resolve()
    summaries=[run(build,out,'cap90-x3',{}),
               run(build,out,'late90-x4',dict(GSF_NEXT_MULTIPLIER='4',GSF_NEXT_TEST_READY_DELAY_MS='45')),
               run(build,out,'deadline-priority',dict(GSF_NEXT_MULTIPLIER='4',
                   GSF_NEXT_REAL_PRIORITY_PACING='1',GSF_NEXT_TEST_READY_DELAY_MS='45')),
               run(build,out,'live',dict(GSF_TEST_LIVE='1'),56),
               run(build,out,'live-diag',dict(GSF_TEST_LIVE='1',GSF_TEST_LIVE_DIAGNOSTICS='1',GSF_TEST_SYNC_VALIDATION='1'),56)]
    (out/'RESULTS.json').write_text(json.dumps(summaries,indent=2)+'\n')
    print('Vulkan live control and API pacing: PASS. Physical scanout UNVERIFIED.')

if __name__=='__main__':main()

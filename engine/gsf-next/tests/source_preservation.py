#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Overlapping Vulkan producer/output: native throughput, complete synthetic output.
Software correctness workload. Not a Steam Deck performance qualification.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def run(build,out,mult,period,hz=90,require_throughput=False):
    name=f'source-{period}-x{mult}';directory=out/name
    if directory.exists():shutil.rmtree(directory)
    directory.mkdir(parents=True)
    env=os.environ.copy()
    env.update(LP_NUM_THREADS='1',VK_LAYER_PATH=str(build/'layer')+':/usr/share/vulkan/explicit_layer.d',
        VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING='1',VK_LOADER_LAYERS_DISABLE='~implicit~',
        AFMF_TEST_X11='1',AFMF_TEST_EXTENT='128x128',AFMF_TEST_FRAMES='100',AFMF_LOG='3',
        AFMF_MIN_FPS='0',AFMF_GOVERNOR='1',AFMF_EXTRA_IMAGES='3',AFMF_PERFORMANCE_MODE='performance',
        AFMF_SEARCH_MODE='standard',AFMF_ACQUIRE_TIMEOUT_US='0',
        GSF_TEST_PIPELINED='1',GSF_TEST_BENCHMARK='1',GSF_TEST_SOURCE_PERIOD_NS=str(period),
        GSF_NEXT_MULTIPLIER=str(mult),GSF_NEXT_ENABLED=str(int(mult!=0)),GSF_NEXT_REFRESH_HZ=str(hz),
        GSF_NEXT_DIAGNOSTICS='1',GSF_NEXT_PRESENT_REPORT=str(directory/'present'),GSF_NEXT_STATUS_DIR=str(directory))
    result=subprocess.run(['xvfb-run','-a',str(build/'afmf_headless')],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
    (directory/'run.log').write_text(result.stdout)
    assert result.returncode==0,(name,result.returncode,result.stdout[-5000:])
    assert 'VUID-' not in result.stdout and 'validation:' not in result.stdout,(name,result.stdout[-5000:])
    rows=[json.loads(line) for p in directory.glob('present.*.jsonl') for line in p.read_text().splitlines()]
    rows.sort(key=lambda r:r['submit_ns'])
    reals=[r for r in rows if r['real']]
    assert len(reals)==100,(name,'lost real frame',len(reals))
    steady=[r for r in rows if r['ordinal']>=21]
    sr=[r for r in steady if r['real']]
    source_fps=(len(sr)-1)*1e9/(sr[-1]['source_b']-sr[0]['source_b'])
    output_fps=(len(steady)-1)*1e9/(steady[-1]['submit_ns']-steady[0]['submit_ns'])
    expected_source=1e9/period
    expected_output=min(expected_source*(mult or 1),hz)
    synthetic=sum(not r['real'] for r in steady)
    expected_g=0
    if mult:
        expected_g=(min(mult,round(period/(1e9/hz)))-1)*len(sr)
    snapshots=[json.loads(p.read_text()) for p in directory.glob('*.json')]
    for row in snapshots:
        assert row.get('dropped_synthetic',0)==0,(name,'unexpected WSI output starvation',row)
        assert row['generated']==row['accepted_synthetic'],(name,'unretired generation',row)
    acceptance=dict(source=source_fps>=expected_source*.94,
        output=expected_output*.92<=output_fps<=expected_output*1.03,
        synthetic=synthetic>=expected_g*.93)
    summary=dict(throughput_acceptance=acceptance,case=name,source_fps=round(source_fps,3),wsi_fps=round(output_fps,3),steady_synthetic=synthetic,displayed=None)
    (directory/'MEASUREMENTS.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary),flush=True)
    if require_throughput: assert all(acceptance.values()),(name,'throughput acceptance FAILED',summary)
    return summary


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build',type=Path,required=True);parser.add_argument('--require-throughput',action='store_true',help='Enforce unchanged 94% source, 92% output and 93% synthetic acceptance; requires a GPU with budget');parser.add_argument('--realtime',action='store_true',help='Require actual 90-Hz software throughput instead of the 3x-dilated correctness clock')
    args=parser.parse_args();build=args.build.resolve();out=build/'Testing/source-preservation'
    scale=1 if args.realtime else 3
    hz=90//scale
    print(f'Cadence clock scale={scale}; actual API budget={hz} Hz, no physical display claim.',flush=True)
    results=[run(build,out,m,22222222*scale,hz,args.require_throughput) for m in (0,2,3,4)]
    results += [run(build,out,3,33333333*scale,hz,args.require_throughput),run(build,out,4,44444444*scale,hz,args.require_throughput)]
    (out/'RESULTS.json').write_text(json.dumps(results,indent=2)+'\n')
    print('Pipelined Vulkan lifecycle: PASS. Throughput acceptance is recorded separately; no hardware qualification.')

if __name__=='__main__':main()

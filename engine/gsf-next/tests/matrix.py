#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual X11 Vulkan, golden pixels, both validation orders and fault/lifetime cases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def check_outputs(directory, count):
    checked=0
    for metadata in sorted(directory.glob('afmf_phases_*.json')):
        m=json.loads(metadata.read_text()); n=m['frame']; phases=m['phases']
        assert len(phases)==count, (metadata, phases, count)
        assert phases==sorted(set(phases)) and all(0<t<1 for t in phases)
        hashes=[]; centres=[]
        for i,t in enumerate(phases):
            path=directory/f'afmf_generated_{n}{"_g"+str(i+1) if i else ""}.ppm'
            head,size,depth,data=path.read_bytes().split(b'\n',3)
            assert head==b'P6' and depth==b'255'
            w,h=map(int,size.split());assert len(data)==w*h*3
            xs=[]; ys=[]
            for k in range(w*h):
                if min(data[k*3:k*3+3])>128: xs.append(k%w);ys.append(k//w)
            assert 2048<len(xs)<8192, (path,len(xs))
            cx=sum(xs)/len(xs);cy=sum(ys)/len(ys)
            expected=64+((n-1)*8)%448+8*t+31.5
            assert abs(cx-expected)<2 and abs(cy-231.5)<2, (path,cx,cy,expected)
            hashes.append(hashlib.sha256(data).hexdigest());centres.append(cx);checked+=1
        assert len(set(hashes))==count, (metadata,'duplicate synthetic pixels')
        assert centres==sorted(centres), (metadata,'reversed temporal pixel order',centres)
    assert checked>=count*2, (directory,'not enough verified intermediate images')
    return checked


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--build',type=Path,required=True);ap.add_argument('--out',type=Path);ap.add_argument('--case',action='append')
    args=ap.parse_args();build=args.build.resolve()
    out=(args.out or build/'Testing/gsf-next').resolve();out.mkdir(parents=True,exist_ok=True)
    cases=[
      ('x2',dict(GSF_NEXT_MULTIPLIER='2'),1),
      ('x3',dict(GSF_NEXT_MULTIPLIER='3'),2),
      ('x4',dict(GSF_NEXT_MULTIPLIER='4'),3),
      ('copy-x2',dict(GSF_NEXT_MULTIPLIER='2',GSF_NEXT_OUTPUT_COPY='1'),1),
      ('copy-x3',dict(GSF_NEXT_MULTIPLIER='3',GSF_NEXT_OUTPUT_COPY='1'),2),
      ('copy-x4',dict(GSF_NEXT_MULTIPLIER='4',GSF_NEXT_OUTPUT_COPY='1'),3),
      ('batch-x3',dict(GSF_NEXT_MULTIPLIER='3',GSF_NEXT_WARP_BATCH='1'),2),
      ('batch-x4',dict(GSF_NEXT_MULTIPLIER='4',GSF_NEXT_WARP_BATCH='1'),3),
      ('batch-hud',dict(GSF_NEXT_MULTIPLIER='4',GSF_NEXT_WARP_BATCH='1',AFMF_TEST_HUD_BAR='1'),0),
      ('batch-validation-first',dict(GSF_NEXT_MULTIPLIER='4',GSF_NEXT_WARP_BATCH='1',GSF_TEST_VALIDATION_FIRST='1'),3),
      ('direct-no-profile',dict(GSF_NEXT_MULTIPLIER='4',AFMF_PROFILE='0',AFMF_LOG='2',GSF_TEST_SYNC_VALIDATION='1'),3),
      ('batch-no-profile',dict(GSF_NEXT_MULTIPLIER='4',GSF_NEXT_WARP_BATCH='1',AFMF_PROFILE='0',AFMF_LOG='2',GSF_TEST_SYNC_VALIDATION='1'),3),
      ('queue1',dict(GSF_NEXT_MULTIPLIER='4',GSF_NEXT_MAX_IN_FLIGHT='1'),3),
      ('queue3-validation-first',dict(GSF_NEXT_MULTIPLIER='3',GSF_NEXT_MAX_IN_FLIGHT='3',GSF_TEST_VALIDATION_FIRST='1'),2),
      ('pan',dict(AFMF_TEST_PAN='1'),0),
      ('hud',dict(AFMF_TEST_HUD_BAR='1'),0),
      ('recreate',dict(AFMF_TEST_RECREATE='6'),None),
      ('no-idle-teardown',dict(AFMF_TEST_NO_IDLE='1'),None),
      ('no-interpolator-real-only',dict(AFMF_INTERPOLATE='0'),None),
      ('overload-hitches',dict(AFMF_TEST_HITCH='4'),None),
      ('pipelined',dict(GSF_TEST_PIPELINED='1',GSF_TEST_SOURCE_PERIOD_NS='33333333'),None),
      ('output-allocation-failure',dict(GSF_NEXT_TEST_FAIL_OUTPUT_ALLOC='1'),None),
      ('submit-device-lost',dict(GSF_NEXT_TEST_FAIL_SUBMIT='1'),None),
    ]
    summaries=[]
    for name,patch,pixels in cases:
        if args.case and name not in args.case: continue
        directory=out/name
        if directory.exists(): shutil.rmtree(directory)
        directory.mkdir(); env=os.environ.copy()
        env.update(VK_LAYER_PATH=str(build/'layer')+':/usr/share/vulkan/explicit_layer.d',
                   VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING='1',VK_LOADER_LAYERS_DISABLE='~implicit~',
                   AFMF_TEST_X11='1',AFMF_LOG='3',AFMF_TEST_FRAMES='16',AFMF_TEST_FRAME_MS='30',
                   AFMF_MIN_FPS='0',AFMF_GOVERNOR='0',AFMF_EXTRA_IMAGES='5',
                   AFMF_PERFORMANCE_MODE='performance',AFMF_SEARCH_MODE='standard',
                   AFMF_ACQUIRE_TIMEOUT_US='16000',GSF_NEXT_REFRESH_HZ='360',
                   GSF_NEXT_STATUS_DIR=str(directory))
        if pixels is not None: env['AFMF_DUMP_DIR']=str(directory)
        env['GSF_NEXT_GPU_REPORT']=str(directory/'gpu.jsonl')
        env.update(patch)
        result=subprocess.run(['xvfb-run','-a',str(build/'afmf_headless')],env=env,
                              stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=90)
        log=result.stdout;(out/(name+'.log')).write_text(log)
        assert 'validation:' not in log and 'VUID-' not in log, (name,'Vulkan validation error',log[-4000:])
        if name=='submit-device-lost':
            assert result.returncode!=0 and ('VkResult -4' in log or 'layer submission failed' in log),(name,log[-3000:])
            assert 'accepted_synthetic=0' in log,(name,'unsafe synthetic present after failure')
        else:
            assert result.returncode==0, (name,result.returncode,log[-4000:])
            assert 'presented 16 frames through VK_LAYER_GSF_next' in log,(name,'lost source frame')
        if name in ('no-interpolator-real-only','output-allocation-failure'):
            assert 'generation off' in log and 'built=0' in log
        elif name!='submit-device-lost':
            totals=re.findall(r'GSF_NEXT_SUMMARY real=(\d+) built=(\d+) accepted_real=(\d+) accepted_synthetic=(\d+)',log)
            assert totals,(name,'no retirement counters')
            assert sum(int(t[2]) for t in totals)==16, (name,totals)
            assert all(t[1]==t[3] for t in totals),(name,'submitted output lost before WSI acceptance',totals)
        if name not in ('submit-device-lost','no-interpolator-real-only','output-allocation-failure') and env.get('AFMF_PROFILE')!='0':
            timed=0
            for file in directory.glob('gpu.jsonl*'):
                seen=set()
                for line in file.read_text().splitlines():
                    record=json.loads(line); count=record['outputs']
                    assert record['pair_b'] not in seen,(name,'GPU pair counted twice')
                    seen.add(record['pair_b'])
                    assert 0<=count<=int(env.get('GSF_NEXT_MULTIPLIER','2'))-1
                    assert len(record['phases'])==count
                    assert record['phases']==sorted(set(record['phases']))
                    assert all(0<t<1 for t in record['phases'])
                    assert record['motion_ms']>=0 and all(v>=0 for v in record['warp_ms'])
                    assert record['motion_ms']+sum(record['warp_ms'])+record.get('batch_ms',0)+record.get('scratch_copy_ms',0)<=record['pair_ms']+.01
                    if name in ('x2','x3','x4'): assert record['direct_output']
                    if name.startswith('copy-'): assert not record['direct_output']
                    if name.startswith('batch-') and count>1: assert record['batch_ms']>0 and sum(record['warp_ms'])==0
                    timed+=1
            assert timed>0,(name,'no GPU timestamp evidence')
        if env.get('AFMF_PROFILE')=='0':
            assert not list(directory.glob('gpu.jsonl*')) and 'GPU time per frame' not in log,(name,'profiling was not disabled')
        checked=check_outputs(directory,pixels) if pixels else 0
        if 'hud' in name:
            format_id=int(re.findall(r'format (\d+), present mode',log)[0])
            assert format_id in (37,43,44,50),format_id
            expected=bytes((0,0,255) if format_id in (44,50) else (255,0,0))
            files=list(directory.glob('afmf_generated_*.ppm'));assert files
            for file in files:
                _,size,_,data=file.read_bytes().split(b'\n',3);w,h=map(int,size.split())
                for y in range(150,350):
                    for x in range(170,172):
                        k=(y*w+x)*3
                        assert data[k:k+3]==expected,(file,x,y,'HUD colour or coverage changed')
                checked+=1
        summaries.append(dict(case=name,exit_code=result.returncode,pixel_checks=checked))
        (out/'RESULTS.json').write_text(json.dumps(summaries,indent=2)+'\n')
        print(name+': PASS'+(' ('+str(checked)+' GPU output images)' if checked else ''),flush=True)
    print('Vulkan X11 matrix: PASS. Software GPU; physical display and Deck performance UNVERIFIED.')

if __name__=='__main__': main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Warm timestamp experiment: baseline, scratch-copy, direct, shared pixel reconstruction.
No scanout/FPS/power claims. Run serially on the same ICD; retain raw samples and logs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess


def percentiles(values):
    values=sorted(values)
    def at(p):
        x=(len(values)-1)*p;i=int(x);j=min(i+1,len(values)-1)
        return round(values[i]+(values[j]-values[i])*(x-i),6)
    return dict(p50=at(.5),p95=at(.95),p99=at(.99))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,required=True);ap.add_argument('--baseline',type=Path)
    ap.add_argument('--out',type=Path,required=True);ap.add_argument('--frames',type=int,default=64)
    ap.add_argument('--repeats',type=int,default=3);ap.add_argument('--extent',action='append');ap.add_argument('--multiplier',type=int,choices=(2,3,4),action='append')
    ap.add_argument('--mode',action='append',choices=('baseline','copy','direct','batch'))
    a=ap.parse_args();assert a.frames>=32 and a.repeats>=1
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);results=[]
    modes=a.mode or (['baseline'] if a.baseline else [])+['copy','direct','batch']
    builds={name:path.resolve() for name,path in [('candidate',a.build)]+([('baseline',a.baseline)] if a.baseline else [])}
    metadata=dict(method='serial interleaved modes; first 16 frames excluded; full requested output counts only',
        frames=a.frames,repeats=a.repeats,threads=os.environ.get('LP_NUM_THREADS'),
        builds={name:dict(library_sha256=hashlib.sha256((path/'layer/libafmf-linux.so').read_bytes()).hexdigest(),
            provenance=json.loads((path/'BASELINE.json').read_text()) if (path/'BASELINE.json').exists() else None)
            for name,path in builds.items()},displayed=None,power_w=None)
    (out/'METHOD.json').write_text(json.dumps(metadata,indent=2)+'\n')
    if 'baseline' in modes:assert a.baseline,'--baseline is required'
    # Interleave modes per repeat to avoid attributing a monotonic warmup/thermal drift to one path.
    for extent in a.extent or ['640x480','1280x800']:
      for mult in a.multiplier or (2,3,4):
       samples={m:[] for m in modes};medians={m:[] for m in modes};coverage={m:[] for m in modes}
       for repeat in range(a.repeats):
        for mode in modes[repeat%len(modes):]+modes[:repeat%len(modes)]:
            build=(a.baseline if mode=='baseline' else a.build).resolve()
            directory=out/f'{extent}-x{mult}-{mode}-{repeat}'
            if directory.exists():shutil.rmtree(directory)
            directory.mkdir();env=os.environ.copy()
            env.update(VK_LAYER_PATH=str(build/'layer')+':/usr/share/vulkan/explicit_layer.d',
                VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING='1',VK_LOADER_LAYERS_DISABLE='~implicit~',
                AFMF_TEST_X11='1',AFMF_TEST_EXTENT=extent,AFMF_TEST_FRAMES=str(a.frames),
                AFMF_TEST_FRAME_MS='30',GSF_TEST_BENCHMARK='1',AFMF_LOG='3',
                AFMF_MIN_FPS='0',AFMF_GOVERNOR='0',AFMF_EXTRA_IMAGES='5',
                AFMF_PERFORMANCE_MODE='performance',AFMF_SEARCH_MODE='standard',AFMF_PROFILE='1',
                GSF_NEXT_DIAGNOSTICS='1',GSF_NEXT_REFRESH_HZ='360',GSF_NEXT_MULTIPLIER=str(mult),
                GSF_NEXT_OUTPUT_COPY=str(int(mode=='copy')),GSF_NEXT_WARP_BATCH=str(int(mode=='batch')),
                GSF_NEXT_GPU_REPORT=str(directory/'gpu'),GSF_NEXT_STATUS_DIR=str(directory))
            for name in ('AFMF_DUMP_DIR','GSF_TEST_PIPELINED','GSF_NEXT_LIVE_CONFIG','GSF_NEXT_PRESENT_REPORT'):
                env.pop(name,None)
            run=subprocess.run(['xvfb-run','-a',str(build/'afmf_headless')],env=env,
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
            (directory/'run.log').write_text(run.stdout)
            assert run.returncode==0,(directory,run.stdout[-4000:])
            totals=re.findall(r'GSF_NEXT_SUMMARY real=(\d+) built=(\d+) accepted_real=(\d+) accepted_synthetic=(\d+)',run.stdout)
            assert totals and sum(int(t[2]) for t in totals)==a.frames and all(t[1]==t[3] for t in totals),(directory,totals)
            rows=[json.loads(line) for file in directory.glob('gpu.*.jsonl') for line in file.read_text().splitlines()]
            warm=[r for r in rows if r['pair_b']>=16]
            rows=[r for r in warm if r['outputs']==mult-1]
            coverage[mode].append(dict(warm_pairs=len(warm),full_pairs=len(rows),excluded_incomplete=len(warm)-len(rows)))
            assert len(rows)>=a.frames//2,(directory,'too few fully generated warm pairs',len(rows))
            if mode!='baseline':
                assert all(r['direct_output']==(mode!='copy') for r in rows)
                assert all((r['batch_ms']>0)==(mode=='batch' and mult>2) for r in rows)
            samples[mode].extend(rows);medians[mode].append(statistics.median(r['pair_ms'] for r in rows))
            print(f'{extent} x{mult} {mode} run {repeat}: {len(rows)} warm pairs, p50 {medians[mode][-1]:.3f} ms',flush=True)
       for mode in modes:
            rows=samples[mode]
            summary=dict(extent=extent,multiplier=mult,mode=mode,pairs=len(rows),coverage=coverage[mode],run_pair_medians_ms=medians[mode],
                pair_ms=percentiles([r['pair_ms'] for r in rows]),motion_ms=percentiles([r['motion_ms'] for r in rows]),
                reconstruction_ms=percentiles([sum(r['warp_ms'])+r.get('batch_ms',0) for r in rows]),
                scratch_copy_ms=percentiles([r.get('scratch_copy_ms',max(0,r['pair_ms']-r['motion_ms']-sum(r['warp_ms']))) for r in rows]),
                displayed=None,power_w=None)
            results.append(summary)
       (out/'RESULTS.json').write_text(json.dumps(results,indent=2)+'\n')
    print('Timestamp comparison complete; physical display, RDNA2 speed and power remain unqualified.')


if __name__=='__main__':main()

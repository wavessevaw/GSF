#!/usr/bin/env python3
"""Functional GSF pipeline acceptance on software Vulkan, never Deck acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native-build',type=Path,required=True)
    p.add_argument('--core-build',type=Path,required=True)
    p.add_argument('--validation-layers',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    native=a.native_build.resolve();core=a.core_build.resolve()
    validation=str(a.validation_layers.resolve())
    base=dict(os.environ,LP_NUM_THREADS='4')
    report=dict(source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                scope='software Vulkan functional acceptance; WSI handoffs, not physical scanout',
                output_hz=9,target_product_hz=90,checks=[])
    for f in [native/'libVkLayer_gfg_native.so',core/'libgfg-temporal.a']:
        report[f.name+'_sha256']=hashlib.sha256(f.read_bytes()).hexdigest()
    def run(name,cmd,env,expected=()):
        trace=(a.out/(name+'.jsonl')).resolve()
        result=subprocess.run([str(x) for x in cmd],env=dict(base,**env,GFG_NATIVE_TELEMETRY=str(trace)),capture_output=True,text=True,timeout=180)
        log=result.stdout+result.stderr
        (a.out/(name+'.log')).write_text('command='+json.dumps([str(x) for x in cmd])+'\n'+log)
        failed_validation=bool(re.search(r'Validation Error|VALIDATION ERROR|VUID-|te validation:',log))
        passed=result.returncode==0 and not failed_validation and all(s in log for s in expected)
        deferred=[];ordered=[];canceled=[]
        if trace.exists():
            pairs=[json.loads(line) for line in trace.read_text().splitlines() if line.strip()]
            deferred=[p for p in pairs if p.get('event')=='pair' and p.get('deferred_g2')]
            ordered=[p for p in deferred if p.get('wsi_accepted_ns',[0])[0] and p.get('g2_submit_return_ns')]
            canceled=[p for p in deferred if p.get('dispatched_outputs')==1 and p.get('compute_mask')==1 and not p.get('g2_submit_return_ns')]
            passed=passed and all(p['g2_submit_return_ns']>=p['wsi_accepted_ns'][0] for p in ordered)
        if name.startswith('pixels-order-'):
            passed=passed and bool(ordered)
        if name.startswith(('duplicate-order-','cut-order-')):
            passed=passed and bool(canceled)
        if name.startswith('x2-pixels-order-'):
            passed=passed and not deferred
        report['checks'].append(dict(name=name,passed=passed,returncode=result.returncode,
            validation_error=failed_validation,deferred_pairs=len(deferred),g1_before_g2_submit=len(ordered),canceled_g2=len(canceled)))
        (a.out/'acceptance.json').write_text(json.dumps(report,indent=2)+'\n')
        print(name,'PASS' if passed else 'FAIL',flush=True)
        if not passed:
            # Public job annotations retain the actual assertion/validation
            # failure even when log downloads require a separate API session.
            details=json.dumps(report['checks'][-1])+'\n'+log[-5500:]
            print('::error title=GSF '+name+'::'+details.replace('%','%25').replace('\r','%0D').replace('\n','%0A'),flush=True)
    run('core-gsf',[core/'te-tests'],dict(VK_LAYER_PATH=validation,VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',GFG_TEST_SHARED_FLOW='4'),['183 checks, 0 failures'])
    run('borrowed-gsf',[native/'borrowed-device'],dict(VK_LAYER_PATH=validation,VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',GFG_TEST_GSF='1'),['device lifetime PASS'])
    env=dict(GFG_NATIVE_FG='1',GFG_NATIVE_GPU_BRIDGE='1',GFG_NATIVE_ASYNC='1',GFG_NATIVE_GPU_SYNC='same-device',
             GFG_NATIVE_ALGO='gsf',GFG_NATIVE_OUTPUT_HZ='9',GFG_NATIVE_TEST_SOURCE_HZ='3',GFG_NATIVE_TRACE='1')
    run('sync-real-only-fallback',[native/'wsi-gpu-smoke'],dict(env,GFG_NATIVE_ASYNC='0',
        VK_INSTANCE_LAYERS='VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation',VK_LAYER_PATH=str(native)+':'+validation,
        GFG_TEST_EXPECT_GPU='1',GFG_TEST_VALIDATION='1',GFG_TEST_FRAMES='4',GFG_TEST_ALLOW_SKIPS='1'),
        ['reason=gsf-requires-output-worker','real=4 synthetic=0 skipped=6','validation errors=0'])
    for i,order in enumerate(['VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation','VK_LAYER_KHRONOS_validation:VK_LAYER_GFG_native']):
        e=dict(env,VK_INSTANCE_LAYERS=order,VK_LAYER_PATH=str(native/'observer-layer')+':'+validation,GFG_NATIVE_TEST_FRAMES='4')
        run(f'pixels-order-{i}',[native/'async-observer-app'],e,['content=PASS handoff_order=PASS steady_pairs=2','spacing=PASS'])
        run(f'x2-pixels-order-{i}',[native/'async-observer-app'],dict(e,GFG_NATIVE_MODE='2'),['content=PASS handoff_order=PASS steady_pairs=2 multiplier=2','spacing=PASS'])
        run(f'duplicate-order-{i}',[native/'async-observer-app'],dict(e,GFG_NATIVE_TEST_DUPLICATE='1'),['duplicate_recovery=PASS real=4 synthetic=0'])
        run(f'cut-order-{i}',[native/'async-observer-app'],dict(e,GFG_NATIVE_TEST_CUT='1'),['real=4 synthetic=0 cut_recovery=PASS','skipped_cut='])
        e=dict(env,VK_INSTANCE_LAYERS=order,VK_LAYER_PATH=str(native)+':'+validation,
               GFG_TEST_EXPECT_GPU='1',GFG_TEST_VALIDATION='1',GFG_TEST_FRAMES='6',GFG_TEST_ALLOW_SKIPS='1')
        for name,extra in [('recreate',dict(GFG_TEST_RECREATE='1')),
                           ('queue-idle',dict(GFG_TEST_QUEUE_IDLE_ONLY='1')),
                           ('unknown',dict(GFG_TEST_UNKNOWN_PRESENT='1',GFG_TEST_ALLOW_SKIPS='1'))]:
            run(f'{name}-order-{i}',[native/'wsi-gpu-smoke'],dict(e,**extra),['validation errors=0','GPU bridge WSI smoke PASS'])
    report['passed']=all(c['passed'] for c in report['checks'])
    (a.out/'acceptance.json').write_text(json.dumps(report,indent=2)+'\n')
    raise SystemExit(0 if report['passed'] else 1)

if __name__=='__main__':main()

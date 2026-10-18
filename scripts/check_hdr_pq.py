#!/usr/bin/env python3
"""Numerical HDR PQ GPU checks; never physical display or Deck performance evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-build',type=Path,required=True)
    parser.add_argument('--validation-layers',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--core-build',type=Path)
    parser.add_argument('--baseline-core',type=Path,help='Optional retained pre-change shader build for byte comparison')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    report=dict(scope='software GPU numerical PQ reconstruction, not HDR WSI or scanout',
                physical_displayed_fps=None,hardware_acceptance='NOT TESTED',checks=[])
    cases=[('gsf-linear-x3',{'GFG_TEST_GSF':'1','GFG_TEST_PQ_LINEAR':'1'}),
           ('gsf-linear-x2',{'GFG_TEST_GSF':'1','GFG_TEST_PQ_LINEAR':'1','GFG_TEST_X2':'1'}),
           ('gsf-duplicate-10bit',{'GFG_TEST_GSF':'1','GFG_TEST_PQ_DUPLICATE':'1'}),
           ('lmc-linear-x3',{'GFG_TEST_GPU_LMC':'1','GFG_TEST_PQ_LINEAR':'1'}),
           ('tiles-old-precision',{}),('lmc-old-precision',{'GFG_TEST_GPU_LMC':'1'})]
    base={k:v for k,v in os.environ.items() if not k.startswith('GFG_TEST_')}
    base.update(VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
                VK_LAYER_PATH=str(args.validation_layers.resolve()),LP_NUM_THREADS='4')
    for name,extra in cases:
        result=subprocess.run([str(args.native_build.resolve()/'hdr-precision')],env=dict(base,**extra),
                              capture_output=True,text=True,timeout=120)
        log=result.stdout+result.stderr;(args.out/(name+'.log')).write_text(log)
        passed=result.returncode==0 and 'PASS; scanout unobserved' in log and not re.search(r'VALIDATION ERROR|Validation Error|VUID-',log)
        report['checks'].append(dict(name=name,passed=passed,returncode=result.returncode,
                                    unsupported_formats=re.findall(r'format=(\d+) BLIT_DST unsupported',log)))
        (args.out/'acceptance.json').write_text(json.dumps(report,indent=2)+'\n')
        print(name,'PASS' if passed else 'FAIL',flush=True)
    if args.baseline_core:
        assert args.core_build,'--baseline-core requires --core-build'
        shaders={}
        for old in sorted((args.baseline_core/'spv').glob('*.spv')):
            new=args.core_build/'spv'/old.name
            shaders[old.name]=dict(identical=new.exists() and old.read_bytes()==new.read_bytes(),
                                  baseline_sha256=hashlib.sha256(old.read_bytes()).hexdigest())
        assert shaders,'no baseline shaders'
        report['legacy_shaders']=shaders
        report['checks'].append(dict(name='legacy-shader-byte-identity',passed=all(x['identical'] for x in shaders.values())))
    report['passed']=all(x['passed'] for x in report['checks'])
    (args.out/'acceptance.json').write_text(json.dumps(report,indent=2)+'\n')
    raise SystemExit(0 if report['passed'] else 1)


if __name__=='__main__':main()

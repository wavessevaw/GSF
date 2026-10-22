#!/usr/bin/env python3
"""Compare completed engine JSON; retain failures, including non-gating climb."""
import argparse
import json
p=argparse.ArgumentParser();p.add_argument('baseline');p.add_argument('candidate');a=p.parse_args()
b=json.load(open(a.baseline));c=json.load(open(a.candidate))
assert b['size']==c['size'] and b['device']==c['device']
left={(s['scene'],q['t']):q for s in b['scenes'] for q in s['phases']}
right={(s['scene'],q['t']):q for s in c['scenes'] for q in s['phases']}
assert left.keys()==right.keys()
rows=[dict(scene=s,t=t,baseline=left[s,t]['psnr_gen'],candidate=right[s,t]['psnr_gen'],delta=round(right[s,t]['psnr_gen']-left[s,t]['psnr_gen'],3),baseline_pass=left[s,t]['pass'],candidate_pass=right[s,t]['pass']) for s,t in left]
print(json.dumps(dict(device=c['device'],baseline_gating_failures=b['failures'],candidate_gating_failures=c['failures'],rows=rows),indent=2))

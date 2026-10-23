#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from the repository root. Compare production scheduler binaries.
import sys,json,tempfile,argparse
ap=argparse.ArgumentParser()
ap.add_argument("--baseline",required=True)
ap.add_argument("--candidate",default="kedavra/build/kfg_sim")
ap.add_argument("--out",required=True)
a=ap.parse_args()
sys.path.insert(0,'kedavra/sched')
import run_scenarios as r
cases=[('x2_60_near',60,2,30,29,34),('x3_90_near',90,3,30,29,34),('x2_90_near',90,2,45,19,23),('x375_90_near',90,3,24,37,43),('x3_90_stable',90,3,30,20,25)]
out=[]
with tempfile.TemporaryDirectory() as d:
 for name,hz,m,cap,ms,p99 in cases:
  sc=dict(name=name,args=f'--refresh {hz} --mult {m} --cap {cap} --game-ms {ms} --game-p99-ms {p99} --spike-prob 0.003 --spike-ms 40',duration=180,mult=m)
  for seed in range(1,4):
   item=dict(name=name,seed=seed)
   for label,sim in [('baseline',a.baseline),('new',a.candidate)]:
    rep,_=r.run(sim,sc,seed,d); item[label]=rep
   out.append(item)
  print(name,[(z,sum(x[z]['hitches_per_min'] for x in out if x['name']==name)/3) for z in ['baseline','new']],flush=True)
json.dump(out,open(a.out,'w'),indent=2)

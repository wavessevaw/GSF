#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Scoped reconstruction of retained game traces. No inferred missing history or scanout."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import statistics
import tarfile


def key(e, seq='te_sequence'):
    return e.get('pid'), e.get('generation'), e.get(seq)


def median(v):
    return statistics.median(v) if v else None


def analyze(events, launch, report):
    pairs=[e for e in events if e.get('event')=='pair']
    submitted={key(p,'te_submit_sequence'):p for p in pairs if p.get('dispatched_outputs') and p.get('te_submit_sequence')}
    if len(submitted)!=sum(bool(p.get('dispatched_outputs')) for p in pairs):
        raise ValueError('Ambiguous submitted pair namespace')
    gpu={key(e):e for e in events if e.get('event')=='gpu_correlated' and key(e) in submitted}
    watches=defaultdict(list)
    for e in events:
        if e.get('event')=='timeline_watch' and key(e) in submitted:watches[key(e)].append(e)
    badphase,mask2,half,phase_rows=0,0,0,[]
    previous={}
    for p in pairs:
        scope=key(p)[:2];a=previous.get(scope);previous[scope]=p
        if not a or a['source1_id']+1!=p['source1_id'] or p['source_discontinuity']:continue
        start,end=a['target_ns'][2],p['target_ns'][2]
        for k in range(p['multiplier']-1):
            if not p['planned_mask']&(1<<k):continue
            target=p['target_ns'][k]
            if not start<target<end:continue
            tau=(target-start)/(end-start);old=(k+1)/p['multiplier']
            if p['multiplier']==3 and p['planned_mask']==2:
                mask2+=1;badphase+=abs(tau-old)>1e-6;half+=abs(tau-.5)<1e-9
            phase_rows.append(dict(pid=p['pid'],generation=p['generation'],source_A=a['source1_id'],source_B=p['source1_id'],
                source_A_ns=a['source1_ns'],source_B_ns=p['source1_ns'],planned_A_ns=start,planned_B_ns=end,
                slot_id=k,target_ns=target,planned_mask=p['planned_mask'],old_tau=old,slot_tau=tau))
    late,samples=0,0
    for k,g in gpu.items():
        if g.get('calibration_status')!='calibrated':continue
        for w in watches[k]:
            if not w.get('deadline_ns'):continue
            samples+=1
            late+=g['gpu_start_cpu_ns']-g['calibration_max_deviation_ns']>w['deadline_ns']
    origin=min(p['worker_start_ns'] for p in pairs);windows=[]
    for bucket in range(int((pairs[-1]['worker_start_ns']-origin)//10_000_000_000)+1):
        ps=[p for p in pairs if bucket*10_000_000_000<=p['worker_start_ns']-origin<(bucket+1)*10_000_000_000]
        if ps:windows.append(dict(start_s=10*bucket,pairs=len(ps),real_accepted=sum(bool(p['wsi_accepted_ns'][2]) for p in ps),
            synthetic_accepted=sum(sum(bool(v) for v in p['wsi_accepted_ns'][:2]) for p in ps),dispatched=sum(p['dispatched_outputs'] for p in ps)))
    return dict(mode=int(launch['native_environment']['GFG_NATIVE_MODE']),source_commit=launch['build']['source_commit'],
        retained_pairs=len(pairs),retained_source_ids=[pairs[0]['source1_id'],pairs[-1]['source1_id']],
        retained_duration_s=(pairs[-1]['worker_start_ns']-origin)/1e9,
        retained_real_accepted=sum(bool(p['wsi_accepted_ns'][2]) for p in pairs),
        retained_synthetic_accepted=sum(sum(bool(v) for v in p['wsi_accepted_ns'][:2]) for p in pairs),
        dispatched_pairs=len(submitted),dispatched_outputs=sum(p['dispatched_outputs'] for p in pairs),
        calibrated_start_delay_median_ms=median([g['submit_to_gpu_start_ns']/1e6 for g in gpu.values() if g.get('calibration_status')=='calibrated']),
        matched_output_deadlines=samples,start_late_with_reported_uncertainty=late,
        x3_mask2_valid_adjacent=mask2,x3_mask2_nominal_phase_wrong=badphase,x3_mask2_exact_midpoint=half,
        admission=dict(Counter(p['admission_decision'] for p in pairs)),windows=windows,
        prior_trace_was_truncated=report.get('telemetry_truncated',False),
        legacy_exit=report.get('exit_code'),physical_unique_displayed=None),phase_rows


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('archive',type=Path);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
    a.out.mkdir(parents=True,exist_ok=True);results=[];rows=[]
    with tarfile.open(a.archive) as t:
        folders=sorted({str(Path(m.name).parent) for m in t.getmembers() if m.name.endswith('/LAUNCH.json')})
        for folder in folders:
            def read(name):
                f=t.extractfile(folder+'/'+name)
                if f is None:raise ValueError(name)
                return f.read()
            events=[];invalid=0
            for line in read('frames.jsonl').splitlines():
                try:
                    e=json.loads(line)
                    if isinstance(e,dict):events.append(e)
                    else:invalid+=1
                except ValueError:invalid+=1
            result,phases=analyze(events,json.loads(read('LAUNCH.json')),json.loads(read('REPORT.json')))
            result['invalid_lines']=invalid;results.append(result);rows.extend(phases)
    output=dict(scope='RETAINED_LOGS_NOT_FULL_SESSION_OR_PHYSICAL_SCANOUT',archive_sha256=hashlib.sha256(a.archive.read_bytes()).hexdigest(),runs=results)
    (a.out/'trace_reconstruction.json').write_text(json.dumps(output,indent=2)+'\n')
    if rows:
        with (a.out/'phase_fixture.csv').open('w') as f:
            w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
    for r in results:print(json.dumps(r,ensure_ascii=False))


if __name__=='__main__':main()

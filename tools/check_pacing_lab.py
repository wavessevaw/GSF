#!/usr/bin/env python3
"""Fail-closed evidence checks; software WSI is never physical scanout proof."""
import argparse, collections, json, re
from pathlib import Path


def owner(path, multiplier, negative=False, mask2=False):
    text=path.read_text()
    assert 'Validation Error' not in text and 'VUID-' not in text, text[-3000:]
    if negative:
        assert 'GSF_LMC_GPU_VERIFY FAIL' in text, 'duplicate negative control did not fail pixel verification'
        assert not re.search(r'GSF_LOADER_LIVE LMC output_present=',text)
        return {'negative_duplicate_rejected':True,'physical_scanout':None}
    assert 'Validation Error' not in text and 'VUID-' not in text, text[-3000:]
    pixels=re.findall(r'GSF_LMC_GPU_VERIFY PASS source_A=(\d+) source_B=(\d+) output=(\d+) tau=([\d.]+)',text)
    count=1 if mask2 else multiplier-1
    expected=[(str(b-1),str(b),str(k)) for b in range(2,9) for k in range(count)]
    assert [row[:3] for row in pixels]==expected, pixels
    assert all(abs(float(row[3])-(0.5 if mask2 else (int(row[2])+1)/multiplier))<2e-6 for row in pixels)
    presents=re.findall(r'GSF_LOADER_LIVE (ORIGINAL|LMC) output_present=(\d+) originals=(\d+) intermediate=(\d+)',text)
    assert [row[0] for row in presents]==['ORIGINAL']+(['LMC']*count+['ORIGINAL'])*7
    assert [int(row[1]) for row in presents]==list(range(1,len(presents)+1))
    assert tuple(map(int,presents[-1][2:]))==(8,7*count)
    assert 'GSF_LMC_OWNER_APP_TERMINAL originals=8' in text
    return dict(multiplier=multiplier,originals=8,gpu_pixel_verified=len(pixels),wsi_accepted=len(presents),physical_scanout=None)


def stress(directory, minimum_seconds):
    events=[];segments=sorted(directory.glob('frames.*.jsonl'))
    assert segments, 'no retained segments'
    for segment in segments:
        raw=segment.read_bytes();assert raw.endswith(b'\n'),segment
        events.extend(json.loads(row) for row in raw.splitlines())
    states=[json.loads(p.read_text()) for p in directory.glob('frames.*.state.json')]
    assert states and all(s['closed'] and not s['dropped_bytes'] and s['stop_reason']=='none' for s in states), states
    pairs=[e for e in events if e.get('event')=='pair']
    assert len(pairs)>100
    keys=[(e['pid'],e['generation'],e['source1_id']) for e in pairs]
    assert len(set(keys))==len(keys), 'duplicate pair identity'
    times=[e['source1_ns'] for e in pairs]
    seconds=(max(times)-min(times))/1e9
    assert seconds>=minimum_seconds-2, seconds
    mapping=[e for e in events if e.get('event')=='slot_mapping']
    # A phase is tied to immutable A/B plan coordinates, not a nominal mode.
    for e in mapping:
        if e.get('dispatch_state')=='recorded_deferred':continue
        # Fields remain schema-versioned; inspect the full event on mismatch.
        a,b,s=e['planned_A_ns'],e['planned_B_ns'],e['slot_target_ns']
        assert a<s<b and abs(e['tau_applied']-(s-a)/(b-a))<2e-6,e
    log=(directory/'game.log').read_text()
    assert 'GPU bridge WSI smoke PASS' in log and 'Validation Error' not in log and 'VUID-' not in log, log[-4000:]
    outcomes=collections.Counter(e['state'] for e in events if e.get('event')=='pacing_recovery')
    return dict(scope='installed normal layer / lavapipe / no shadow',seconds=seconds,segments=len(segments),pairs=len(pairs),slot_mappings=len(mapping),recovery_states=dict(outcomes),physical_scanout=None)


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('kind',choices=['owner','stress']);ap.add_argument('path',type=Path)
    ap.add_argument('--multiplier',type=int,default=2);ap.add_argument('--negative',action='store_true');ap.add_argument('--mask2',action='store_true');ap.add_argument('--seconds',type=int,default=600)
    args=ap.parse_args()
    result=owner(args.path,args.multiplier,args.negative,args.mask2) if args.kind=='owner' else stress(args.path,args.seconds)
    print(json.dumps(result,sort_keys=True))

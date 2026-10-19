#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize local software evidence without extrapolating to RDNA2."""
import argparse
import json
from pathlib import Path
import re


def percentile(values,p):
    values=sorted(values)
    if not values:return '—'
    position=(len(values)-1)*p;lower=int(position);upper=min(lower+1,len(values)-1)
    return f'{values[lower]+(values[upper]-values[lower])*(position-lower):.3f}'


def main():
    ap=argparse.ArgumentParser();ap.add_argument('matrix',type=Path);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
    results=json.loads((a.matrix/'RESULTS.json').read_text());assert len(results)==12
    lines=['# GSF Next: software verification report','',
      'This is lavapipe/llvmpipe (LLVM 15, two CPU worker threads), Xvfb/X11, Ubuntu 22.04, GCC 11.4; **not Steam Deck/RDNA2 performance**.',
      'Output 640×480, half-resolution flow 320×240, five pyramid levels. Sixteen uploaded real frames per case, Vulkan validation, explicit GPU pixel readbacks and detailed timestamp output. This is a correctness workload with cold-start and CPU scheduling effects, not a real-time game benchmark.',
      '', 'All 12 integration cases passed; the display-slot planner independently checks the 60/90/120 Hz scenarios, changing refresh, hitches and fractional source cadence. The software WSI matrix uses a 360 Hz admission budget to ensure a 30 ms source interval can request every synthetic phase; Xvfb is not a 360 Hz physical display.',
      '', '| Mode | Retired pairs with outputs | Pair p50 / p95 / p99, ms | Shared analysis p50, ms | G1 / G2 / G3 p50, ms | Built / WSI accepted |',
      '| --- | ---: | --- | ---: | --- | --- |']
    for case in ('x2','x3','x4'):
        records=[]
        for file in (a.matrix/case).glob('gpu.jsonl*'):
            records.extend(json.loads(line) for line in file.read_text().splitlines())
        records=[r for r in records if r['outputs']>0]
        assert records
        log=(a.matrix/(case+'.log')).read_text()
        real,built,accepted_real,accepted=re.findall(r'GSF_NEXT_SUMMARY real=(\d+) built=(\d+) accepted_real=(\d+) accepted_synthetic=(\d+)',log)[-1]
        pair=[r['pair_ms'] for r in records];motion=[r['motion_ms'] for r in records]
        warps=[percentile([r['warp_ms'][i] for r in records if r['outputs']>i],.5) for i in range(3)]
        lines.append('| '+case+' | '+str(len(records))+' | '+' / '.join(percentile(pair,p) for p in (.5,.95,.99))+' | '+percentile(motion,.5)+' | '+' / '.join(warps)+' | '+built+' / '+accepted+' |')
    checks=sum(r['pixel_checks'] for r in results)
    lines.extend(['',f'{checks} generated GPU images passed temporal-position, unique-pixel-hash and temporal-order checks. The upstream centroid tolerance stays ±2 pixels; moving-block flow and HUD checks are retained. Empty display reservations are valid, so the harness checks actual reserved warm pairs instead of assuming that a particular source ordinal always generated an output.',
      '', 'Cases: ×2, ×3, ×4; queue limits 1/2/3; validation above/below the layer; pan; HUD; recreation; teardown without application idle; missing interpolator real-only fallback; source hitches; submit/DEVICE_LOST fault propagation. Constructed outputs must all retire into successful WSI acceptance in non-fault tests. Startup and unavailable-slot skips remain visible and are not counted as interpolation.',
      '', 'The split compute/present family path is implemented with per-queue host synchronization. Lavapipe exposes one shared queue, so independent RADV queue families, Gamescope FROG ordering, Steam CEF, suspend/resume, games, HDR, physical presentation, power, input latency and native game FPS remain unqualified.',
      '', 'The old Flow Lite 0.1.13 runtime is deliberately not loaded. No performance superiority over Flow Lite, ECO45, LSFG or MAKO is inferred from these CPU-driver timings. There are no comparative Deck runs or 10/15 W power results. Hardware acceptance follows DECK_PROTOCOL.ru.md.',''])
    a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text('\n'.join(lines));print(a.out)

if __name__=='__main__':main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inject a producer dependency in a real Vulkan APP, never in the GSF layer.

Software reproduction of CPU Present preceding GPU input readiness. This is
not a shader-cost benchmark, GPU saturation, Deck root-cause proof or scanout.
"""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess


def numbers(line):
    return {k: float(v) for k, v in re.findall(r'(\w+)=(-?\d+(?:\.\d+)?)', line)}


def median(values):
    return round(statistics.median(values), 4) if values else None


def analyze(events, start, end):
    origins = {(p['pid'], p['generation'], p['te_sequence']): p for p in events
               if p.get('event') == 'pair' and p.get('dispatched_outputs')}
    records = []
    for e in events:
        if e.get('event') != 'gpu_correlated' or e.get('calibration_status') != 'calibrated':
            continue
        p = origins.get((e['pid'], e['generation'], e['te_sequence']))
        if p is None:
            raise ValueError('Unmatched calibrated GPU observation')
        frame = p['source1_id']-1
        if start <= frame < end:
            records.append((p, e))
    pairs = [p for p in events if p.get('event') == 'pair' and start <= p['source1_id']-1 < end]
    return {'source_pairs': len(pairs),
            'dispatched': sum(p['dispatched_outputs'] for p in pairs),
            'wsi_accepted': sum(bool(p['wsi_accepted_ns'][0]) for p in pairs),
            'calibrated_records': len(records),
            'submit_to_gpu_start_median_ms': median([e['submit_to_gpu_start_ns']/1e6 for _, e in records])}


def run(build, output, order):
    output.mkdir(parents=True, exist_ok=True)
    cases = [
        # name, layer enabled, admission enabled, source Hz, frames, delay range
        ('healthy', True, True, 5, 30, 0, 0),
        ('baseline-delayed', False, False, 45, 180, 0, 180),
        ('old-admission-delayed', True, False, 45, 180, 0, 180),
        ('new-admission-delayed', True, True, 45, 180, 0, 180),
        ('delay-then-recovery', True, True, 45, 450, 30, 140),
    ]
    result = {'scope': 'SOFTWARE_APP_RENDER_DEPENDENCY_NOT_DECK_OR_SCANOUT',
              'order': order, 'cases': [], 'status': 'FAIL'}
    try:
        for name, active, admission, hz, frames, start, end in cases:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('GFG_NATIVE_', 'GFG_TEST_', 'DISABLE_GFG_NATIVE_'))}
            env.update(VK_LAYER_PATH=str(build)+':/usr/share/vulkan/explicit_layer.d',
                       GFG_TEST_VALIDATION='1', GFG_TEST_FRAMES=str(frames),
                       GFG_TEST_SOURCE_HZ=str(hz), GFG_TEST_ABSOLUTE_PACING='1',
                       GFG_TEST_ALLOW_SKIPS='1', GFG_TEST_WIDTH='96', GFG_TEST_HEIGHT='64',
                       GFG_TEST_RENDER_READY_DELAY_MS='35' if end else '0',
                       GFG_TEST_RENDER_READY_FROM=str(start), GFG_TEST_RENDER_READY_UNTIL=str(end))
            layers = ['VK_LAYER_GFG_native', 'VK_LAYER_KHRONOS_validation'] if active else ['VK_LAYER_KHRONOS_validation']
            if order == 'validation-first':
                layers.reverse()
            env['VK_INSTANCE_LAYERS'] = ':'.join(layers)
            trace = output/(name+'.jsonl')
            if active:
                env.update(GFG_TEST_EXPECT_GPU='1', GFG_NATIVE_FG='1', GFG_NATIVE_GPU_BRIDGE='1',
                           GFG_NATIVE_GPU_SYNC='same-device', GFG_NATIVE_ASYNC='1',
                           GFG_NATIVE_FORCE_FIFO='1', GFG_NATIVE_MODE='2', GFG_NATIVE_ALGO='lmc-gpu',
                           GFG_NATIVE_LMC_TIER='2', GFG_NATIVE_PACING='1', GFG_NATIVE_OUTPUT_HZ='90',
                           GFG_NATIVE_VISIBILITY_PROBE='1', GFG_NATIVE_TIMELINE_WATCH='1',
                           GFG_NATIVE_CALIBRATED_TIMESTAMPS='1',
                           GFG_NATIVE_LATENCY_ADMISSION='1' if admission else '0',
                           GFG_NATIVE_TELEMETRY=str(trace))
            p = subprocess.run([str(build/'wsi-gpu-smoke')], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, timeout=90)
            (output/(name+'.log')).write_text(p.stdout)
            assert p.returncode == 0, (name, p.stdout[-7000:])
            assert 'validation errors=0' in p.stdout, name
            row = {'name': name, 'frames': frames, 'source_hz': hz,
                   'gate_from': start, 'gate_until': end}
            row['source_rate'] = numbers(next(l for l in p.stdout.splitlines() if l.startswith('GSF_SOURCE_RATE ')))
            source_markers = [numbers(l) for l in p.stdout.splitlines() if l.startswith('GSF_RENDER_READY_SOURCE ')]
            row['application_submit_to_present_median_ms'] = median(
                [(m['present_ns']-m['submit_ns'])/1e6 for m in source_markers if m['injected_delay_ms']])
            if end:
                gate = numbers(next(l for l in p.stdout.splitlines() if l.startswith('GSF_RENDER_READY_GATE ')))
                assert gate['armed'] == gate['released'] == end-start, (name, gate)
            if active:
                stats = numbers(next(l for l in p.stdout.splitlines() if l.startswith('GFG native: gpu stats ')))
                assert stats['real'] == frames and stats['errors'] == 0, (name, stats)
                assert stats['present_path_host_pixel_reads'] == stats['present_path_cpu_waits'] == 0, stats
                events = [json.loads(l) for l in trace.read_text().splitlines()]
                row.update(stats=stats, all=analyze(events, 0, frames))
                assert row['all']['calibrated_records'] > 0, 'Calibration unavailable; cannot establish this experiment'
                if name == 'healthy':
                    assert stats['synthetic'] >= 10, row
                elif name.endswith('delayed'):
                    row['steady'] = analyze(events, 60, frames)
                    # Require genuinely asynchronous producer readiness, not
                    # an app-side vkQueueSubmit wait disguised as GPU latency.
                    assert row['application_submit_to_present_median_ms'] < 17.5, row
                    assert row['steady']['submit_to_gpu_start_median_ms'] is not None, row
                    assert row['steady']['submit_to_gpu_start_median_ms'] >= 20, row
                    if admission:
                        assert row['steady']['dispatched'] <= 4, row
                    else:
                        assert row['steady']['dispatched'] >= 8, row
                else:
                    row['delayed'] = analyze(events, start, end)
                    row['recovered'] = analyze(events, 320, frames)
                    assert row['recovered']['wsi_accepted'] > 0, row
            result['cases'].append(row)
            (output/'RESULT.json').write_text(json.dumps(result, indent=2)+'\n')
            print('READINESS_CASE', json.dumps(row), flush=True)
        result['status'] = 'PASS'
    except Exception as e:
        result['error'] = str(e)
        raise
    finally:
        (output/'RESULT.json').write_text(json.dumps(result, indent=2)+'\n')
    print('RENDER_READINESS_LAB_PASS: real app dependency, preserved originals, calibrated latency, bounded retries, recovery; Deck cause unproven')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--order', choices=('native-first', 'validation-first'), required=True)
    a = p.parse_args()
    run(a.build.resolve(), a.out.resolve(), a.order)

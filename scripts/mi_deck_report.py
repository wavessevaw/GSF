#!/usr/bin/env python3
"""Summarize GSF telemetry without publishing paths, argv or raw user logs."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path


def percentiles(values):
    values = sorted(values)
    if not values:
        return None
    return {name: round(values[int((len(values)-1)*q)], 6)
            for name, q in (('p50', .5), ('p95', .95), ('p99', .99), ('max', 1))}


def analyze(root):
    launch = json.loads((root/'LAUNCH.json').read_text())
    report = json.loads((root/'REPORT.json').read_text())
    env = launch['native_environment']
    events = [json.loads(line) for p in sorted(root.glob('frames*.jsonl'))
              for line in p.open() if line.strip()]
    pairs = [e for e in events if e.get('event') == 'pair']
    gpu = [e for e in events if e.get('event') == 'gpu_pair']
    outputs = [e for e in events if e.get('event') == 'gpu_output']
    starts = [e for e in events if e.get('event') == 'gpu_correlated'
              and e.get('submit_to_gpu_start_ns') is not None]
    slots = [e for e in events if e.get('event') == 'synthetic_lifecycle']
    sensors = [json.loads(line) for line in (root/'amdgpu.jsonl').open() if line.strip()]
    last = report.get('last_live', [{}])[0]
    result = {
        'source_commit': launch['build']['source_commit'],
        'multiplier': int(env['GFG_NATIVE_MODE']),
        'quality': env.get('GFG_NATIVE_RECONSTRUCTION_QUALITY'),
        'buffer_requested': int(env.get('GFG_NATIVE_BUFFER_DEPTH', 0)),
        'buffer_effective_last': last.get('buffer_depth'),
        'independent_owner_last': last.get('buffered_owner'),
        'source_pairs': len(pairs),
        'planned_synthetic': sum(e['planned'] for e in slots),
        'dispatched_synthetic': sum(e['dispatched'] for e in slots),
        'wsi_accepted_synthetic': sum(bool(e['wsi_accepted_ns']) for e in slots),
        'admission': dict(Counter(e['admission_decision'] for e in pairs)),
        'synthetic_drop_reasons': dict(Counter(e['reason'] for e in slots)),
        'source_interval_ms': percentiles([e['interval_ns']/1e6 for e in pairs]),
        'shared_refinement_ms': percentiles([e['analysis_ms'][2] for e in gpu]),
        'pair_first_ready_gpu_ms': percentiles([e['ready_ms'][0] for e in gpu]),
        'per_output_render_ms': percentiles([e['render_ms'] for e in outputs]),
        'submit_to_gpu_start_ms': percentiles([e['submit_to_gpu_start_ns']/1e6 for e in starts]),
        'source_age_at_worker_ms': percentiles([(e['worker_start_ns']-e['source1_ns'])/1e6 for e in pairs]),
        'global_gpu_busy_percent': percentiles([e['gpu_busy_percent'] for e in sensors if isinstance(e.get('gpu_busy_percent'), (int, float))]),
        'global_power_w': percentiles([e['power_w'] for e in sensors if isinstance(e.get('power_w'), (int, float))]),
        'power_scope': 'sensor reading, not generator-only power or configured TDP',
        'baseline_source_fps_degradation': None,
        'physical_displayed_fps': None,
    }
    # Join by pair sequence; store only relative durations for replay tests.
    by_seq = {e.get('te_submit_sequence'): e for e in pairs if e.get('dispatched_outputs')}
    spans = {e['te_sequence']: e for e in gpu}
    trace = []
    for start in starts:
        seq = start['te_sequence']
        pair, span = by_seq.get(seq), spans.get(seq)
        if not pair or not span:
            continue
        k = next((i for i in range(3) if pair['compute_mask'] & (1 << i)), None)
        # Legacy pair arrays store G1/G2/real; G3 is in lifecycle records.
        if k is None or k >= 2:
            continue
        trace.append((result['multiplier'], pair['interval_ns'], pair['refresh_period_ns'],
                      start['submit_to_gpu_start_ns'], round(span['ready_ms'][0]*1e6),
                      pair['target_ns'][k]-pair['refresh_period_ns']//4-pair['compute_admission_ns']))
    return result, trace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--trace', type=Path)
    args = parser.parse_args()
    results, traces = [], []
    for root in sorted(args.root.iterdir()):
        if (root/'LAUNCH.json').is_file():
            result, trace = analyze(root)
            results.append(result)
            # A compact, evenly sampled regression fixture, no raw timestamps.
            traces.extend(trace[i*(len(trace)-1)//127] for i in range(128) if trace)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({'sessions': results, 'hardware_after_fix': 'NOT_MEASURED'}, indent=2)+'\n')
    if args.trace:
        with args.trace.open('w') as f:
            writer = csv.writer(f)
            writer.writerow(('multiplier', 'source_interval_ns', 'refresh_ns', 'gpu_start_delay_ns', 'gpu_span_ns', 'unbuffered_slack_ns'))
            writer.writerows(traces)


if __name__ == '__main__':
    main()

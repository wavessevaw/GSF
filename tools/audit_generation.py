#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay GSF game evidence. No simulated FPS, physical-display or fix prediction."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import zipfile

FIXTURE_FIELDS = ('archive', 'offset_ns', 'source_to_submit_ns', 'deadline_after_submit_ns',
                  'gpu_start_after_submit_ns', 'gpu_span_ns', 'max_deviation_ns',
                  'synthetic_wsi_accepted', 'deadline_kind', 'source_interval_ns', 'refresh_period_ns')

def median(values):
    return round(statistics.median(values), 6) if values else None


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    return values[min(len(values)-1, int((len(values)-1)*q))]


class SubmissionIndex:
    def __init__(self, pairs):
        self.pairs = pairs
        self.indices = defaultdict(list)
        for i, p in enumerate(pairs):
            seq = p.get('te_submit_sequence', p.get('te_sequence', 0))
            if seq and p.get('dispatched_outputs', 0):
                self.indices[seq].append(i)

    def find(self, event):
        found = [i for i in self.indices[event.get('te_sequence', 0)]
                 if all(event.get(k) is None or event[k] == self.pairs[i].get(k)
                        for k in ('pid', 'generation'))]
        return (found[0], 'matched') if len(found) == 1 else (None, 'ambiguous' if found else 'unmatched')


def timing_summary(rows):
    return {'samples': len(rows),
            'start_definitely_after_deadline': sum(r['gpu_start_after_submit_ns'] - r['max_deviation_ns'] >
                                                   r['deadline_after_submit_ns'] for r in rows),
            'estimated_ready_after_deadline': sum(r['gpu_start_after_submit_ns'] + r['gpu_span_ns'] -
                                                 r['max_deviation_ns'] > r['deadline_after_submit_ns'] for r in rows),
            'source_to_submit_median_ms': median([r['source_to_submit_ns']/1e6 for r in rows]),
            'submit_to_start_median_ms': median([r['gpu_start_after_submit_ns']/1e6 for r in rows]),
            'gpu_span_median_ms': median([r['gpu_span_ns']/1e6 for r in rows]),
            'deadline_slack_after_submit_median_ms': median([r['deadline_after_submit_ns']/1e6 for r in rows]),
            'start_lateness_median_ms': median([(r['gpu_start_after_submit_ns']-r['deadline_after_submit_ns'])/1e6 for r in rows]),
            'estimated_ready_lateness_median_ms': median([(r['gpu_start_after_submit_ns']+r['gpu_span_ns']-
                                                           r['deadline_after_submit_ns'])/1e6 for r in rows]),
            'shader_zero_cost_control': 'Observed start is held fixed; NOT a causal FPS replay or promised fix'}


def analyze_events(events, tag='trace'):
    pairs = [e for e in events if e.get('event') == 'pair']
    if not pairs:
        return {'pairs': 0}, []
    first = min(p['source1_ns'] for p in pairs)
    last = max(p['source1_ns'] for p in pairs)
    index = SubmissionIndex(pairs)
    gpu, watch = defaultdict(list), defaultdict(list)
    joins = {k: Counter() for k in ('gpu_pair', 'gpu_correlated', 'timeline_watch')}
    feedback = {}
    for e in events:
        kind = e.get('event')
        if kind == 'compositor_feedback':
            key = (e.get('pid'), e.get('generation'), e.get('present_id'))
            feedback[key] = e.get('actual_ns')
        if kind not in joins:
            continue
        i, status = index.find(e)
        joins[kind][status] += 1
        if status == 'matched' and kind == 'gpu_pair':
            gpu[i].append(e)
        if status == 'matched' and kind == 'timeline_watch' and e.get('output_index') == 0:
            watch[i].append(e)
    windows = defaultdict(lambda: {'source_pairs': 0, 'synthetic_dispatched': 0, 'synthetic_wsi_accepted': 0})
    accepted_offsets, steady_pairs, steady_dispatch, steady_accepted = [], 0, 0, 0
    for p in pairs:
        offset = p['source1_ns']-first
        accepted = sum(bool(v) for v in p.get('wsi_accepted_ns', [])[:2])
        w = windows[offset//10000000000]
        w['source_pairs'] += 1
        w['synthetic_dispatched'] += p.get('dispatched_outputs', 0)
        w['synthetic_wsi_accepted'] += accepted
        if accepted:
            accepted_offsets.append(offset/1e9)
        if offset >= 60000000000:
            steady_pairs += 1
            steady_dispatch += p.get('dispatched_outputs', 0)
            steady_accepted += accepted
    rows, missing_duration_or_deadline, ready_after_real = [], 0, []
    for e in events:
        if e.get('event') != 'gpu_correlated' or e.get('calibration_status') != 'calibrated':
            continue
        i, status = index.find(e)
        if status != 'matched':
            continue
        p = pairs[i]
        if len(gpu[i]) != 1 or len(watch[i]) != 1 or p.get('multiplier') != 2:
            missing_duration_or_deadline += 1
            continue
        g, w = gpu[i][0], watch[i][0]
        if not g.get('timed') or not g.get('ready_ms') or not w.get('deadline_ns'):
            missing_duration_or_deadline += 1
            continue
        start, returned = e.get('gpu_start_cpu_ns'), e.get('submit_return_ns')
        if start is None or returned is None:
            continue
        span = round(g['ready_ms'][0]*1000000)
        row = {'archive': tag, 'offset_ns': p['source1_ns']-first,
               'source_to_submit_ns': returned-p['source1_ns'],
               'deadline_after_submit_ns': w['deadline_ns']-returned,
               'gpu_start_after_submit_ns': start-returned, 'gpu_span_ns': span,
               'max_deviation_ns': e['calibration_max_deviation_ns'],
               'synthetic_wsi_accepted': int(bool(p.get('wsi_accepted_ns', [0])[0])),
               'deadline_kind': w.get('deadline_kind', 'unknown'),
               'source_interval_ns': p.get('interval_ns', 0),
               'refresh_period_ns': p.get('refresh_period_ns', 0)}
        rows.append(row)
        ids = p.get('google_present_ids', [])
        actual = feedback.get((p.get('pid'), p.get('generation'), ids[2])) if len(ids) > 2 else None
        if actual and row['offset_ns'] >= 60000000000:
            ready_after_real.append((start+span-actual)/1e6)
    steady = [r for r in rows if r['offset_ns'] >= 60000000000]
    result = {'pairs': len(pairs), 'source_span_s': (last-first)/1e9,
              'bridge_scopes': len({(p.get('pid'), p.get('generation')) for p in pairs}),
              'synthetic_dispatched': sum(p.get('dispatched_outputs', 0) for p in pairs),
              'synthetic_wsi_accepted': sum(sum(bool(v) for v in p.get('wsi_accepted_ns', [])[:2]) for p in pairs),
              'first_accepted_offset_s': min(accepted_offsets) if accepted_offsets else None,
              'last_accepted_offset_s': max(accepted_offsets) if accepted_offsets else None,
              'refresh_period_counts': dict(Counter(p.get('refresh_period_ns') for p in pairs)),
              'joins': {k: dict(v) for k, v in joins.items()},
              'missing_duration_or_deadline': missing_duration_or_deadline,
              'calibrated_all': timing_summary(rows),
              'post_60s': {'source_pairs': steady_pairs, 'dispatched': steady_dispatch,
                           'synthetic_wsi_accepted': steady_accepted,
                           'dispatch_rate_per_s': steady_dispatch/((last-first)/1e9-60) if last-first > 60000000000 else None,
                           'timing': timing_summary(steady)},
              'reported_compositor_ready_minus_real_ms': {
                  'samples': len(ready_after_real), 'median': median(ready_after_real),
                  'ready_after_reported_real': sum(v > 0 for v in ready_after_real),
                  'scope': 'Estimated GPU completion vs compositor-reported actual time; NOT physical scanout'},
              'windows': [dict(start_s=k*10, **v) for k, v in sorted(windows.items())],
              'physical_2x': None}
    return result, rows


def analyze_archive(path):
    with zipfile.ZipFile(path) as z:
        def read(name):
            found = [n for n in z.namelist() if Path(n).name == name]
            if len(found) > 1:
                raise ValueError('Ambiguous member '+name)
            if not found:
                return None
            info = z.getinfo(found[0])
            if info.file_size > 256*1024*1024:
                raise ValueError('Oversized evidence '+name)
            return z.read(found[0]).decode('utf-8', errors='replace')
        launch = json.loads(read('LAUNCH.json') or '{}')
        report = json.loads(read('REPORT.json') or '{}')
        log = read('game.log') or ''
        trace = read('frames.jsonl')
        events, invalid = [], 0
        for line in (trace or '').splitlines():
            try:
                e = json.loads(line)
                if isinstance(e, dict):
                    events.append(e)
                else:
                    invalid += 1
            except ValueError:
                invalid += 1
        gates = [dict(re.findall(r'(\w+)=([^ ]+)', line)) for line in log.splitlines()
                 if line.startswith('GSF_REAL_WSI_GATE ')]
        result, rows = analyze_events(events, path.name)
        result.update(archive=path.name, sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                      appid=launch.get('appid'), source_commit=launch.get('build', {}).get('source_commit'),
                      installed_diagnostic_mode=report.get('diagnostic_mode'),
                      legacy_result=report.get('result'), exit_code=report.get('exit_code'),
                      trace_present=trace is not None, invalid_json_lines=invalid,
                      event_counts=dict(Counter(e.get('event') for e in events)), gates=gates,
                      bridge_active_lines=log.count('GPU bridge active:'),
                      generation_started_lines=log.count('generation_started'))
        result['blockers'] = []
        if gates and all(g.get('candidate') == '0' for g in gates):
            result['blockers'].append('ACTIVATION_REFUSED: '+', '.join(sorted({g.get('reason', 'unknown') for g in gates})))
        if result.get('post_60s', {}).get('timing', {}).get('start_definitely_after_deadline', 0):
            result['blockers'].append('OBSERVED_GPU_START_AFTER_DEADLINE: shader-zero-cost control still misses those slots')
        if not trace:
            result['blockers'].append('NO_GENERATION_TRACE: no TE timing or recovery claim possible')
        return result, rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('archives', nargs='+', type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--fixture', type=Path)
    a = p.parse_args()
    results, rows = [], []
    for archive in a.archives:
        result, measurements = analyze_archive(archive)
        results.append(result)
        rows.extend(measurements)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps({'scope': 'HARDWARE_LOG_AUDIT_NOT_CAUSAL_FIX_REPLAY_OR_SCANOUT',
                                'runs': results}, indent=2, sort_keys=True)+'\n')
    if a.fixture:
        a.fixture.parent.mkdir(parents=True, exist_ok=True)
        with a.fixture.open('w') as f:
            w = csv.DictWriter(f, fieldnames=FIXTURE_FIELDS, lineterminator='\n')
            w.writeheader()
            w.writerows(rows)
    for r in results:
        print(r['archive'], 'pairs', r['pairs'], 'WSI synthetic', r.get('synthetic_wsi_accepted', 0), r['blockers'])


if __name__ == '__main__':
    main()

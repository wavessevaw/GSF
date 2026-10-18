"""Windowed, offline GSF telemetry. Observations are not physical-scanout proof."""
import json
import statistics


def _median(values):
    return round(statistics.median(values), 3) if values else None


def diagnostic_summary(path):
    totals = {'threshold_reached': 0, 'expired': 0, 'submit_failed': 0,
              'teardown': 0, 'device_error': 0, 'watch_queue_overflow': 0}
    counts = {'planned_slots': 0, 'submitted_outputs': 0, 'accepted_outputs': 0,
              'gpu_budget_drops': 0}
    windows, sequence_window, gpu_events = {}, {}, []
    overruns = []
    first_ns = None
    try:
        with path.open(encoding='utf-8', errors='replace') as stream:
            for line in stream:
                try:
                    event = json.loads(line)
                except (ValueError, TypeError):
                    continue
                kind = event.get('event')
                if kind == 'timeline_watch':
                    outcome = event.get('watch_end_reason')
                    if outcome in totals:
                        totals[outcome] += 1
                    if outcome == 'threshold_reached' and event.get('deadline_ns'):
                        overruns.append(int(event['completion_overrun_ns']) / 1e6)
                elif kind == 'timeline_watch_overflow':
                    totals['watch_queue_overflow'] += 1
                elif kind == 'gpu_pair':
                    gpu_events.append(event)
                elif kind == 'pair':
                    source = int(event.get('source1_ns', 0) or event.get('worker_start_ns', 0))
                    if first_ns is None:
                        first_ns = source
                    offset = max(0, (source - first_ns) // 10000000000)
                    if offset > 360:
                        continue
                    window = windows.setdefault(offset, {
                        'start_s': offset * 10, 'end_s': (offset + 1) * 10,
                        'source_presents': 0, 'planned_slots': 0, 'dispatched': 0,
                        'accepted': 0, 'gpu_budget_drops': 0, 'readiness_deadlines': 0,
                        '_readiness_wait_ms': [], '_gpu_ready_ms': []})
                    mask = int(event.get('planned_mask', 0))
                    planned = (mask & 1 != 0) + (mask & 2 != 0)
                    dispatched = int(event.get('dispatched_outputs', 0))
                    accepted = sum(int(v) > 0 for v in event.get('wsi_accepted_ns', [])[:2])
                    budget = sum(v == 'gpu-budget' for v in event.get('drop_detail', []))
                    window['source_presents'] += 1
                    window['planned_slots'] += planned
                    window['dispatched'] += dispatched
                    window['accepted'] += accepted
                    window['gpu_budget_drops'] += budget
                    window['readiness_deadlines'] += sum(v == 'gpu-readiness-deadline' for v in event.get('drop_detail', []))
                    for v in event.get('readiness_wait_ns', []):
                        if v:
                            window['_readiness_wait_ms'].append(int(v) / 1000000)
                    for key, v in (('planned_slots', planned), ('submitted_outputs', dispatched),
                                   ('accepted_outputs', accepted), ('gpu_budget_drops', budget)):
                        counts[key] += v
                    seq = int(event.get('te_sequence', 0))
                    if seq:
                        sequence_window[seq] = offset
    except OSError:
        return {'available': False}
    for gpu in gpu_events:
        index = sequence_window.get(int(gpu.get('te_sequence', 0)))
        if index is not None and index in windows and gpu.get('timed'):
            v = gpu.get('ready_ms', [0])[0]
            if v and 0 < float(v) < 10000:
                windows[index]['_gpu_ready_ms'].append(float(v))
    admission_bins = {k: {'windows': 0, 'planned_slots': 0, 'dispatched': 0, 'accepted': 0}
                      for k in ('sparse', 'mixed', 'viable')}
    result_windows = []
    for _, window in sorted(windows.items()):
        frame_count = window['source_presents']
        ratio = window['planned_slots'] / frame_count if frame_count else 0
        classification = 'sparse' if ratio < 0.05 else 'viable' if ratio >= 0.15 else 'mixed'
        b = admission_bins[classification]
        b['windows'] += 1
        for target, field in (('planned_slots', 'planned_slots'), ('dispatched', 'dispatched'), ('accepted', 'accepted')):
            b[target] += window[field]
        window['admission_class'] = classification
        window['planned_slots_per_source'] = round(ratio, 4)
        window['source_present_per_s'] = round(frame_count / 10, 2)
        window['gpu_ready_median_ms'] = _median(window.pop('_gpu_ready_ms'))
        window['readiness_wait_median_ms'] = _median(window.pop('_readiness_wait_ms'))
        window['conversion_dispatched_to_wsi'] = round(window['accepted'] / window['dispatched'], 4) if window['dispatched'] else None
        result_windows.append(window)
    for group in admission_bins.values():
        group['conversion_dispatched_to_wsi'] = round(group['accepted']/group['dispatched'], 4) if group['dispatched'] else None
    report = {'available': True, **totals, **counts,
              'conversion_dispatched_to_wsi': round(counts['accepted_outputs'] / counts['submitted_outputs'], 4)
                  if counts['submitted_outputs'] else None,
              'gpu_queue_wait_ms': None,  # no calibrated CPU/GPU clock in 0.1.5
              'windows_10s': result_windows, 'admission_bins': admission_bins}
    if overruns:
        ordered = sorted(overruns)
        report['completion_overrun_median_ms'] = _median(ordered)
        report['post_deadline_tail_p95_ms'] = round(ordered[min(len(ordered)-1, int((len(ordered)-1)*0.95))], 3)
        report['late_completion_rate'] = round(sum(x > 0 for x in overruns) / len(overruns), 4)
    return report

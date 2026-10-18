"""Windowed, offline GSF telemetry. Observations are not physical-scanout proof."""
import json
import statistics


def _median(values):
    return round(statistics.median(values), 3) if values else None


class _SubmissionIndex:
    """Index actual synthetic submissions, never the last TE counter on a bypass.

    Older correlated events have no process/swapchain identity. Resolve those
    only when the submission number identifies exactly one origin in the run.
    """

    def __init__(self):
        self.sequences = {}

    def add(self, event, offset):
        sequence = int(event.get('te_submit_sequence', event.get('te_sequence', 0)))
        if not sequence or not int(event.get('dispatched_outputs', 0)):
            return
        identity = (event.get('pid'), event.get('generation'))
        origins = self.sequences.setdefault(sequence, {})
        if identity in origins and origins[identity] != offset:
            origins[identity] = None  # conflicting origins must not be guessed
        else:
            origins[identity] = offset

    def find(self, event):
        origins = self.sequences.get(int(event.get('te_sequence', 0)), {})
        candidates = [offset for identity, offset in origins.items()
                      if all(event.get(field) is None or event[field] == identity[i]
                             for i, field in enumerate(('pid', 'generation')))]
        if not candidates:
            return None, 'unmatched'
        if len(candidates) != 1 or candidates[0] is None:
            return None, 'ambiguous'
        return candidates[0], 'matched'


def diagnostic_summary(path):
    totals = {'threshold_reached': 0, 'expired': 0, 'submit_failed': 0,
              'teardown': 0, 'device_error': 0, 'watch_queue_overflow': 0}
    counts = {'planned_slots': 0, 'submitted_outputs': 0, 'accepted_outputs': 0,
              'gpu_budget_drops': 0}
    windows, gpu_events = {}, []
    submissions = _SubmissionIndex()
    joins = {kind: dict.fromkeys(('matched', 'unmatched', 'ambiguous'), 0)
             for kind in ('gpu_pair', 'gpu_correlated')}
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
                    submissions.add(event, offset)
    except OSError:
        return {'available': False}
    for gpu in gpu_events:
        index, status = submissions.find(gpu)
        joins['gpu_pair'][status] += 1
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
              'gpu_queue_wait_ms': None,  # never report an unproven pure queue wait
              'windows_10s': result_windows, 'admission_bins': admission_bins,
              'gpu_sample_joins': joins}
    if overruns:
        ordered = sorted(overruns)
        report['completion_overrun_median_ms'] = _median(ordered)
        report['post_deadline_tail_p95_ms'] = round(ordered[min(len(ordered)-1, int((len(ordered)-1)*0.95))], 3)
        report['late_completion_rate'] = round(sum(x > 0 for x in overruns) / len(overruns), 4)

    # Correlate only after pair/te_sequence windows were indexed. Never
    # infer pure queue wait from this submit-to-GPU-start duration.
    by_window = {int(row['start_s']//10): row for row in result_windows}
    delays = []
    statuses = {}
    calibrated_records = 0
    try:
        with path.open(encoding='utf-8', errors='replace') as stream:
            for line in stream:
                if '"gpu_correlated"' not in line and '"calibration_init"' not in line:
                    continue
                try:
                    event = json.loads(line)
                except (TypeError, ValueError):
                    continue
                if event.get('event') == 'calibration_init':
                    report['calibration_init_status'] = event.get('status', 'unavailable')
                    report['calibration_extension'] = event.get('extension', 'none')
                if event.get('event') != 'gpu_correlated':
                    continue
                calibrated_records += 1
                reason = event.get('calibration_status', 'unknown')
                statuses[reason] = statuses.get(reason, 0) + 1
                offset, status = submissions.find(event)
                joins['gpu_correlated'][status] += 1
                wait_ns = event.get('submit_to_gpu_start_ns')
                if wait_ns is not None and reason == 'calibrated':
                    ms = int(wait_ns)/1e6
                    delays.append(ms)
                    if offset in by_window:
                        by_window[offset].setdefault('_submit_to_gpu_start_ms', []).append(ms)
    except OSError:
        pass
    report['submit_to_gpu_start_median_ms'] = _median(delays)
    report['submit_to_gpu_start_samples'] = len(delays)
    report['gpu_correlated_records'] = calibrated_records
    report['calibration_status_counts'] = statuses

    samples = []
    try:
        with path.with_name('amdgpu.jsonl').open(encoding='utf-8', errors='replace') as stream:
            for line in stream:
                try:
                    event = json.loads(line)
                except (TypeError, ValueError):
                    continue
                if event.get('event') == 'amdgpu_sample':
                    samples.append(event)
    except OSError:
        pass
    busy = [e['gpu_busy_percent'] for e in samples if e.get('gpu_busy_percent') is not None]
    report['amdgpu_sample_count'] = len(samples)
    report['gpu_busy_median_percent'] = _median(busy)
    report['gpu_busy_scope'] = 'global_gpu_not_game'
    for sample in samples:
        if first_ns is None or sample.get('gpu_busy_percent') is None:
            continue
        offset = (int(sample['sample_ns'])-first_ns)//10000000000
        if offset in by_window:
            by_window[offset].setdefault('_gpu_busy_percent', []).append(sample['gpu_busy_percent'])
    for window in result_windows:
        window['submit_to_gpu_start_median_ms'] = _median(window.pop('_submit_to_gpu_start_ms', []))
        window['gpu_busy_median_percent'] = _median(window.pop('_gpu_busy_percent', []))
    # A/B comparisons need an independent baseline with matching scene/thermals.
    report['source_fps_loss_vs_baseline_percent'] = None
    return report

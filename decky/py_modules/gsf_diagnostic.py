"""Offline diagnostic summary of GSF timeline events (not a scanout assertion)."""
import json
import statistics


def diagnostic_summary(path):
    totals = {'threshold_reached': 0, 'expired': 0, 'submit_failed': 0,
              'teardown': 0, 'device_error': 0, 'watch_queue_overflow': 0}
    overruns = []
    counts = {'planned_frames': 0, 'submitted_outputs': 0, 'accepted_outputs': 0,
              'gpu_budget_drops': 0}
    try:
        with path.open(encoding='utf-8', errors='replace') as stream:
            for line in stream:
                try:
                    event = json.loads(line)
                except (ValueError, TypeError):
                    continue
                if event.get('event') == 'timeline_watch':
                    outcome = event.get('watch_end_reason')
                    if outcome in totals:
                        totals[outcome] += 1
                    if outcome == 'threshold_reached' and event.get('deadline_ns'):
                        overruns.append(int(event['completion_overrun_ns']) / 1e6)
                elif event.get('event') == 'timeline_watch_overflow':
                    totals['watch_queue_overflow'] += 1
                elif event.get('event') == 'pair':
                    counts['planned_frames'] += (int(event.get('planned_mask', 0)) & 1 != 0) + (int(event.get('planned_mask', 0)) & 2 != 0)
                    counts['submitted_outputs'] += int(event.get('dispatched_outputs', 0))
                    counts['accepted_outputs'] += sum(v > 0 for v in event.get('wsi_accepted_ns', [])[:2])
                    counts['gpu_budget_drops'] += sum(x == 'gpu-budget' for x in event.get('drop_detail', []))
    except OSError:
        return {'available': False}
    result = {'available': True, **totals, **counts,
              'conversion_dispatched_to_wsi': round(counts['accepted_outputs'] / counts['submitted_outputs'], 4)
                  if counts['submitted_outputs'] else None}
    if overruns:
        ordered = sorted(overruns)
        result['completion_overrun_median_ms'] = round(statistics.median(ordered), 3)
        result['post_deadline_tail_p95_ms'] = round(ordered[min(len(ordered)-1, int((len(ordered)-1)*0.95))], 3)
        result['late_completion_rate'] = round(sum(x > 0 for x in overruns) / len(overruns), 4)
    return result

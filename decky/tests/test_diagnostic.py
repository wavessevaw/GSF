import importlib.util
import json
from pathlib import Path
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"py_modules"))
import unittest

MODULE=Path(__file__).resolve().parents[1]/'py_modules/gsf_diagnostic.py'
spec=importlib.util.spec_from_file_location('gsf_diag',MODULE)
d=importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


class DiagnosticJoinTests(unittest.TestCase):
    def summarize(self, events):
        with tempfile.TemporaryDirectory() as td:
            frame = Path(td)/'frames.jsonl'
            frame.write_text(''.join(json.dumps(e)+'\n' for e in events))
            return d.diagnostic_summary(frame)

    def pair(self, sequence, seconds, pid=11, generation=1, dispatched=1, **extra):
        return {'event': 'pair', 'pid': pid, 'generation': generation,
                'te_sequence': sequence, 'source1_ns': (100+seconds)*10**9,
                'dispatched_outputs': dispatched, **extra}

    def gpu(self, sequence, ms, pid=11, generation=1):
        return {'event': 'gpu_pair', 'pid': pid, 'generation': generation,
                'te_sequence': sequence, 'timed': 1, 'ready_ms': [ms, 0]}

    def correlated(self, sequence, ms, **identity):
        return {'event': 'gpu_correlated', 'te_sequence': sequence,
                'calibration_status': 'calibrated',
                'submit_to_gpu_start_ns': ms*10**6, **identity}

    def test_bypass_does_not_move_completed_job_to_later_window(self):
        # GPU sampling can precede the pair record. The old last-TE counter
        # is repeated on source-only, history-only and failed submissions.
        report = self.summarize([
            self.gpu(9, 4), self.correlated(9, 6), self.pair(9, 0),
            self.pair(9, 11, dispatched=0),
            self.pair(9, 21, dispatched=0),
        ])
        windows = report['windows_10s']
        self.assertEqual(windows[0]['gpu_ready_median_ms'], 4)
        self.assertEqual(windows[0]['submit_to_gpu_start_median_ms'], 6)
        for window in windows[1:]:
            self.assertIsNone(window['gpu_ready_median_ms'])
            self.assertIsNone(window['submit_to_gpu_start_median_ms'])

    def test_process_and_swapchain_namespaces(self):
        report = self.summarize([
            self.pair(1, 0),
            self.pair(1, 11, pid=22),
            self.pair(1, 21, generation=2),
            self.gpu(1, 4), self.gpu(1, 5, pid=22),
            self.gpu(1, 6, generation=2),
            self.correlated(1, 7, pid=11, generation=1),
            self.correlated(1, 8, pid=22, generation=1),
            self.correlated(1, 9, pid=11, generation=2),
        ])
        self.assertEqual([w['gpu_ready_median_ms'] for w in report['windows_10s']], [4, 5, 6])
        self.assertEqual([w['submit_to_gpu_start_median_ms'] for w in report['windows_10s']], [7, 8, 9])
        self.assertEqual(report['gpu_sample_joins']['gpu_correlated']['matched'], 3)

    def test_legacy_ambiguous_identity_is_not_guessed(self):
        report = self.summarize([
            self.pair(1, 0), self.pair(1, 11, generation=2),
            self.correlated(1, 9), self.correlated(1, 8, pid=11),
            self.correlated(99, 7),
        ])
        self.assertEqual(report['gpu_sample_joins']['gpu_correlated'],
                         {'matched': 0, 'ambiguous': 2, 'unmatched': 1})
        self.assertTrue(all(w['submit_to_gpu_start_median_ms'] is None
                            for w in report['windows_10s']))

    def test_explicit_no_submission_cannot_reuse_legacy_counter(self):
        report = self.summarize([
            self.pair(9, 0, te_submit_sequence=9),
            # Inconsistent old count must not override the explicit field.
            self.pair(9, 11, te_submit_sequence=0),
            self.gpu(9, 4), self.correlated(9, 6, pid=11, generation=1),
        ])
        self.assertEqual(report['windows_10s'][0]['gpu_ready_median_ms'], 4)
        self.assertIsNone(report['windows_10s'][1]['gpu_ready_median_ms'])

    def test_conflicting_duplicate_origin_is_ambiguous(self):
        report = self.summarize([
            self.pair(9, 0), self.pair(9, 11), self.pair(9, 21),
            self.gpu(9, 4),
        ])
        self.assertEqual(report['gpu_sample_joins']['gpu_pair']['ambiguous'], 1)
        self.assertTrue(all(w['gpu_ready_median_ms'] is None for w in report['windows_10s']))

    def test_calibrated_join_and_busy(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            frame=root/'frames.jsonl'
            with frame.open('w') as f:
                for event in [
                    {'event':'calibration_init','status':'enabled','extension':'KHR'},
                    {'event':'pair','te_sequence':9,'source1_ns':100000000000,
                     'planned_mask':1,'dispatched_outputs':1,
                     'wsi_accepted_ns':[1,0,1],'drop_detail':[]},
                    {'event':'gpu_correlated','te_sequence':9,'submit_to_gpu_start_ns':4600000,'calibration_status':'calibrated'},
                    {'event':'gpu_correlated','te_sequence':10,'submit_to_gpu_start_ns':None,'calibration_status':'maxDeviation_over_limit'}
                ]:
                    f.write(json.dumps(event)+'\n')
            (root/'amdgpu.jsonl').write_text(json.dumps({
                'event':'amdgpu_sample','sample_ns':100100000000,'gpu_busy_percent':87})+'\n')
            report=d.diagnostic_summary(frame)
            self.assertEqual(report['calibration_extension'],'KHR')
            self.assertEqual(report['submit_to_gpu_start_median_ms'],4.6)
            self.assertEqual(report['submit_to_gpu_start_samples'],1)
            self.assertEqual(report['calibration_status_counts']['maxDeviation_over_limit'],1)
            self.assertEqual(report['gpu_busy_median_percent'],87)
            self.assertEqual(report['windows_10s'][0]['submit_to_gpu_start_median_ms'],4.6)
            self.assertIsNone(report['gpu_queue_wait_ms'])
            self.assertIsNone(report['source_fps_loss_vs_baseline_percent'])

    def test_absent_gpu_and_uncalibrated(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            (root/'frames.jsonl').write_text('{}\n')
            report=d.diagnostic_summary(root/'frames.jsonl')
            self.assertIsNone(report['submit_to_gpu_start_median_ms'])
            self.assertIsNone(report['gpu_busy_median_percent'])


if __name__=='__main__':
    unittest.main()

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

MODULE=Path(__file__).resolve().parents[1]/'py_modules/gsf_diagnostic.py'
spec=importlib.util.spec_from_file_location('gsf_diag',MODULE)
d=importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


class DiagnosticJoinTests(unittest.TestCase):
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

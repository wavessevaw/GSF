# SPDX-License-Identifier: GPL-3.0-or-later
"""Recorded Deck failures and evidence attribution, not simulated generation."""
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('audit', ROOT/'tools/audit_generation.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class EvidenceAudit(unittest.TestCase):
    def test_submission_scope_and_bypass_do_not_reattribute_gpu_work(self):
        pairs = [dict(pid=1, generation=2, te_submit_sequence=7, dispatched_outputs=1),
                 dict(pid=1, generation=2, te_submit_sequence=7, dispatched_outputs=0),
                 dict(pid=1, generation=3, te_submit_sequence=7, dispatched_outputs=1)]
        index = audit.SubmissionIndex(pairs)
        self.assertEqual(index.find(dict(te_sequence=7)), (None, 'ambiguous'))
        self.assertEqual(index.find(dict(te_sequence=7, pid=1, generation=2)), (0, 'matched'))
        self.assertEqual(index.find(dict(te_sequence=7, pid=9)), (None, 'unmatched'))
        self.assertEqual(index.find(dict(te_sequence=99)), (None, 'unmatched'))

    def test_recorded_deadlines_are_missed_before_compute_starts(self):
        with (ROOT/'tests/fixtures/deck_generation_deadlines.csv').open() as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(len(rows), 1661)
        expected = {'GSF-Game-Latest(4).zip': (826, 824, 826),
                    'GSF-Game-Latest(5).zip': (73, 73, 73),
                    'GSF-Game-Latest(7).zip': (21, 21, 21)}
        for archive, counts in expected.items():
            subset = [{k: int(v) if k not in ('archive', 'deadline_kind') else v for k, v in r.items()}
                      for r in rows if r['archive'] == archive and int(r['offset_ns']) >= 60_000_000_000]
            result = audit.timing_summary(subset)
            self.assertEqual((result['samples'], result['start_definitely_after_deadline'],
                              result['estimated_ready_after_deadline']), counts)
            # Even a zero-duration shader misses these recorded, unchanged slots.
            zero_cost = [dict(r, gpu_span_ns=0) for r in subset]
            self.assertEqual(audit.timing_summary(zero_cost)['estimated_ready_after_deadline'], counts[1])
        report = json.loads((ROOT/'docs/audit/20261019/game_evidence.json').read_text())
        active = [r for r in report['runs'] if r.get('pairs')]
        self.assertEqual(len(active), 4)
        self.assertTrue(all(r['post_60s']['synthetic_wsi_accepted'] == 0 for r in active))
        self.assertTrue(all(r['physical_2x'] is None for r in active))

    def test_format_gate_and_empty_fixture_are_reported_without_fake_timings(self):
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory)
            archive = d/'blocked.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('game.log', 'GSF_REAL_WSI_GATE candidate=0 reason=gamescope-packed10-physical-dispatch-unverified\n')
                z.writestr('REPORT.json', '{}')
            p = subprocess.run([sys.executable, str(ROOT/'tools/audit_generation.py'), str(archive),
                                '--out', str(d/'report.json'), '--fixture', str(d/'empty.csv')],
                               capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            result = json.loads((d/'report.json').read_text())['runs'][0]
            self.assertEqual(result['pairs'], 0)
            self.assertFalse(result['trace_present'])
            self.assertTrue(any(v.startswith('ACTIVATION_REFUSED') for v in result['blockers']))
            self.assertNotIn('post_60s', result)
            with (d/'empty.csv').open() as f:
                self.assertEqual(list(csv.DictReader(f)), [])


if __name__ == '__main__':
    unittest.main()

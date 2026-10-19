# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualification must exercise recovery without hiding any malformed output."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('gsf_candidate',ROOT/'engine/gfg-native/candidate/run-test.py')
candidate=importlib.util.module_from_spec(spec);spec.loader.exec_module(candidate)

class CandidateCadenceTests(unittest.TestCase):
    def fixture(self,last=50):
        rows=[]
        for source in range(1,51):
            g=1_000_000_000+source*200_000_000;b=g+100_000_000
            rows.append(dict(event='pair',source1_id=source,refresh_period_ns=100_000_000,
                target_ns=[g,0,b],wsi_accepted_ns=[g if 30<source<=last else 0,0,b]))
        return rows

    def check(self,rows):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'frames.jsonl';path.write_text(''.join(json.dumps(row)+'\n' for row in rows))
            return candidate.check_cadence(path,minimum=18,qualifying_after=30)

    def test_cold_misses_remain_visible_and_steady_generation_is_required(self):
        report=self.check(self.fixture())
        self.assertEqual(report['pairs'],20)
        self.assertEqual(report['qualifying_pairs'],20)
        self.assertEqual(report['qualifying_after_source'],30)
        with self.assertRaises(ValueError):self.check(self.fixture(last=32))

    def test_bad_pair_cannot_hide_in_warmup(self):
        rows=self.fixture()
        bad=json.loads((ROOT/'docs/frame-pacing-audit/evidence/committed_B_pressure_failure.json').read_text())['pairs'][-1]
        rows[10]=bad
        with self.assertRaisesRegex(ValueError,'неверным интервалом'):self.check(rows)

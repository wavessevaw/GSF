# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard the evidence parser against green reports from partial/bad timing data."""
import copy
import math
import unittest
from startup_cache_lab import PAIRS, PASSES, summarize, validate_rows


class StartupEvidence(unittest.TestCase):
    def setUp(self):
        self.rows = [dict(algorithm=2, outputs=2, timed=1, pair=k, a=3*k, b=3*k+3,
                          cpu_submit_ms=1, cpu_total_ms=10, first_ready_ms=5,
                          pair_ready_ms=9, pyramid_ms=0.5,
                          lmc_ms=dict.fromkeys(PASSES, 1)) for k in range(PAIRS)]

    def test_complete_evidence(self):
        validate_rows(self.rows)

    def test_missing_pair_is_rejected(self):
        with self.assertRaises(ValueError):
            validate_rows(self.rows[:-1])

    def test_missing_and_nonfinite_gpu_times_are_rejected(self):
        for value in (None, -1, math.nan, math.inf):
            rows = copy.deepcopy(self.rows)
            rows[0]['lmc_ms']['gpu_model'] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_rows(rows)

    def test_wrong_algorithm_count_source_or_timing_order_is_rejected(self):
        for field, value in (('algorithm', 1), ('outputs', 1), ('timed', 0),
                             ('pair', 1), ('a', 3), ('first_ready_ms', 20),
                             ('cpu_submit_ms', 20)):
            rows = copy.deepcopy(self.rows)
            rows[0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_rows(rows)

    def test_unused_tile_pipeline_and_missing_init_profile_are_rejected(self):
        for log in ('', '[gfg-te] init_profile phase=context-total pipeline=-1 variant=2 host_ms=1 result=0\n'
                    '[gfg-te] init_profile phase=temporal-compute-pipeline pipeline=-1 variant=-1 host_ms=1 result=0'):
            with self.subTest(log=log), self.assertRaises(ValueError):
                summarize(self.rows, log, 20)


if __name__ == '__main__':
    unittest.main()

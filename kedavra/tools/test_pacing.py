import unittest
import kfg_pacing as p

P=10_000_000  # exact fixed100Hz makes the expected slot counts unambiguous

def rows(indices):
    return [dict(vblank=str(i),flip_ns=str(i*P),refresh_ns=str(P),kind='real',
                 content=str(i),src_a=str(i),src_b='0',phase_num='0',phase_den='1',
                 reason='',state='active') for i in indices]

class PacingRegression(unittest.TestCase):
    def test_half_refresh_counts_absent_slots(self):
        r=p.analyze(rows(range(0,200,2)),skip_s=0)
        self.assertAlmostEqual(r['missed_fraction'],99/199)
        self.assertFalse(r['checks']['missed_below_0.5pct'])
    def test_regular_cadence_has_no_misses(self):
        r=p.analyze(rows(range(200)),skip_s=0)
        self.assertEqual(r['missed_fraction'],0)
        self.assertTrue(r['pass'])
    def test_long_unexplained_stall_is_not_excluded(self):
        r=p.analyze(rows([0,1,101,102]),skip_s=0)
        self.assertGreater(r['missed_fraction'],0.9)
        self.assertEqual(r['interval_error_ms']['max'],990)
    def test_explicit_exclusion_breaks_interval_chain(self):
        r=p.analyze(rows([0,1,5,6,7]),skip_s=0,exclude=[(.015,.045)])
        self.assertEqual(r['missed_fraction'],0)
        self.assertEqual(r['interval_error_ms']['max'],0)
    def test_phase_fractions_are_reduced(self):
        rs=rows(range(12))
        for i,r in enumerate(rs):
            r.update(kind='gen',src_b=str(i//3),phase_num='1' if i%3==0 else '2',
                     phase_den='3' if i%3==0 else '6')
        result=p.analyze(rs,skip_s=0)
        self.assertEqual(result['generated_per_interval'],{'1':2})
    def test_quarter_phases_keep_original_source_span(self):
        rs=rows(range(15))
        for i,r in enumerate(rs):
            r.update(kind='gen',src_b=str(i//3),phase_num=str(i%3+1),phase_den='4')
        result=p.analyze(rs,skip_s=0,expect_mult=4)
        self.assertEqual(result['generated_per_locked_interval'],{'3':3})

    def test_invalid_refresh_cannot_be_accepted(self):
        rs=rows(range(3));rs[1]['refresh_ns']='nan'
        self.assertIn('error',p.analyze(rs,skip_s=0))

if __name__=='__main__':unittest.main()

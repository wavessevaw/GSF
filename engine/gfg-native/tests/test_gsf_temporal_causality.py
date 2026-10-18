#!/usr/bin/env python3
"""GSF interpolation causality REFERENCE MATH; not actual WSI/Deck timing.

A at t=0, B at t=T, output delay D; synth j of m at D+j*T/m
must be ready only after B+G_j. Treat ready latency G_j as already
including GPU generation; compositor/display margins belong in G_j.
"""
from fractions import Fraction
import unittest

NS = 1_000_000_000

def source_period(fps: int) -> Fraction:
    if fps <= 0:
        raise ValueError("fps")
    return Fraction(NS, fps)

def lower_bound(source_fps: int, multiplier: int, ready_after_b_ns) -> Fraction:
    if multiplier < 2 or len(ready_after_b_ns) != multiplier - 1:
        raise ValueError("one readiness value required per synthetic output")
    T = source_period(source_fps)
    delta = T / multiplier
    if any(g < 0 for g in ready_after_b_ns):
        raise ValueError("readiness must be nonnegative")
    return max([Fraction(0)] + [T + Fraction(g) - j * delta
                               for j, g in enumerate(ready_after_b_ns, 1)])

def feasible(source_fps: int, multiplier: int, delay_ns, ready_after_b_ns) -> bool:
    if multiplier < 2 or len(ready_after_b_ns) != multiplier - 1:
        raise ValueError("bad multiplier/readiness")
    T = source_period(source_fps)
    delta = T / multiplier
    return all(Fraction(delay_ns) + j * delta >= T + Fraction(g)
               for j, g in enumerate(ready_after_b_ns, 1))

class TemporalCausalityContract(unittest.TestCase):
    def test_30_to_90_lower_bound_is_two_display_intervals(self):
        T = source_period(30)
        d = lower_bound(30, 3, [0, 0])
        self.assertEqual(d, T * Fraction(2, 3))
        self.assertLess(d, T)
        self.assertTrue(feasible(30, 3, d, [0, 0]))
        self.assertFalse(feasible(30, 3, d - 1, [0, 0]))

    def test_45_to_90_lower_bound_is_one_display_interval(self):
        d = lower_bound(45, 2, [0])
        self.assertEqual(d, source_period(45) / 2)
        self.assertTrue(feasible(45, 2, d, [0]))
        self.assertFalse(feasible(45, 2, 0, [0]))

    def test_first_gpu_pair_ready_after_three_ms(self):
        g = Fraction(3_000_000)
        d = lower_bound(30, 3, [g, g])
        self.assertEqual(d, source_period(30) * Fraction(2, 3) + g)
        self.assertTrue(feasible(30, 3, d, [g, g]))

    def test_second_synthetic_can_be_new_bottleneck(self):
        ready = [3_000_000, 20_000_000]
        d = lower_bound(30, 3, ready)
        self.assertEqual(d, source_period(30) + 20_000_000 -
                         source_period(30) * Fraction(2, 3))
        self.assertTrue(feasible(30, 3, d, ready))
        self.assertFalse(feasible(30, 3, d - 1, ready))

    def test_explicit_compositor_margin_increases_bound(self):
        baseline = lower_bound(30, 3, [3_000_000, 3_000_000])
        with_margin = lower_bound(30, 3, [5_000_000, 5_000_000])
        self.assertEqual(with_margin - baseline, 2_000_000)

    def test_30_to_60_two_x(self):
        d = lower_bound(30, 2, [0])
        self.assertEqual(d, source_period(30) / 2)
        self.assertLess(d, source_period(30))

    def test_invalid_input_rejected(self):
        for fps, mul, ready in ((30, 3, [0]), (30, 1, []),
                                 (30, 3, [0, -1]), (0, 3, [0, 0])):
            with self.subTest(fps=fps, mul=mul, ready=ready):
                with self.assertRaises(ValueError):
                    lower_bound(fps, mul, ready)

if __name__ == "__main__":
    unittest.main(verbosity=2)

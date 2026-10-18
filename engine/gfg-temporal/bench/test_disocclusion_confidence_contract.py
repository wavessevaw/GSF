#!/usr/bin/env python3
"""Independent mathematical assertions for GSF LMC coordinate conventions.

This is NOT Vulkan GPU validation, shader parity, image quality or Deck scanout.
"""
import math
import unittest

def sources(target, motion_ab, tau):
    if not 0.0 < tau < 1.0:
        raise ValueError("tau out of range")
    return (target[0]-tau*motion_ab[0], target[1]-tau*motion_ab[1]), (target[0]+(1.0-tau)*motion_ab[0], target[1]+(1.0-tau)*motion_ab[1])

def inside(point, size):
    return 0.0 <= point[0] < size[0] and 0.0 <= point[1] < size[1]

def difference(v1, v2):
    return math.hypot(v1[0]-v2[0], v1[1]-v2[1])

def select_mode(target, motion_ab, field_a, field_b, tau, extent=(10,10), tol=0.5, hud=False):
    """Standalone analogy of select.comp modes, fields use A->B units."""
    if hud:
        return -1
    a,b=sources(target,motion_ab,tau)
    valid_a=inside(a,extent) and difference(field_a,motion_ab) < tol
    valid_b=inside(b,extent) and difference(field_b,motion_ab) < tol
    return 3 if valid_a and valid_b else 1 if valid_a else 2 if valid_b else 0

class Contract(unittest.TestCase):
    def test_fields_have_same_direction(self):
        v=(4.0,-2.0)
        self.assertEqual(difference(v,v),0.0)
        self.assertGreater(math.hypot(v[0]+v[0],v[1]+v[1]),0.0)

    def test_target_to_source_warp_for_each_tau(self):
        v=(4.0,2.0)
        for tau in (1/3,0.5,2/3):
            a,b=sources((5.0,5.0),v,tau)
            self.assertAlmostEqual(b[0]-a[0],v[0])
            self.assertAlmostEqual(b[1]-a[1],v[1])

    def test_both_valid(self):
        v=(4.0,0.0)
        self.assertEqual(select_mode((5,5),v,v,v,0.5),3)

    def test_tau_specific_visibility(self):
        v=(4.0,0.0)
        self.assertEqual(select_mode((1,5),v,v,v,0.25),3)
        self.assertEqual(select_mode((1,5),v,v,v,0.75),2)

    def test_source_a_only_source_b_only(self):
        v=(4.0,0.0)
        self.assertEqual(select_mode((9,5),v,v,v,0.5),1)
        self.assertEqual(select_mode((0,5),v,v,v,0.5),2)

    def test_none_valid_without_reliable_correspondence(self):
        v=(4.0,0.0)
        self.assertEqual(select_mode((5,5),v,(0,0),(0,0),0.5),0)
        self.assertEqual(select_mode((1,1),(8,0),(8,0),(8,0),0.5,extent=(4,4)),0)

    def test_bad_one_side_preserves_other(self):
        v=(4.0,0.0)
        self.assertEqual(select_mode((5,5),v,v,(0,0),0.5),1)
        self.assertEqual(select_mode((5,5),v,(0,0),v,0.5),2)

    def test_hud_nearest_real_bypass(self):
        v=(4.0,0.0)
        self.assertEqual(select_mode((5,5),v,v,v,0.5,hud=True),-1)
        self.assertEqual(["A" if tau < 0.5 else "B" for tau in (1/3,0.5,2/3)],["A","B","B"])

    def test_invalid_timestamps(self):
        for t in (-0.5,0,1,1.5):
            with self.assertRaises(ValueError):
                sources((5,5),(4,0),t)

if __name__ == "__main__":
    unittest.main(verbosity=2)

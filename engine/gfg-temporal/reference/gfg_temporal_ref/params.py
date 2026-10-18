"""Temporal Engine tuning parameters shared by the reference and the Vulkan engine.

The C header ``engine/gfg-temporal/src/te_params.h`` carries the same defaults;
``tests/test_temporal_reference.py`` checks that both lists agree.
Luma/colour thresholds are in normalised units (1.0 == 255 code values).
"""
from __future__ import annotations

from dataclasses import dataclass, asdict

CLASS_REUSE, CLASS_LIGHT, CLASS_FULL = 0, 1, 2
CLASS_NAMES = {CLASS_REUSE: "reuse", CLASS_LIGHT: "light", CLASS_FULL: "full"}

# Experimental mechanisms (spec letters).  None is on by default.
EXPERIMENTS = {
    "A": "checkerboard FULL decisions (odd pixels reuse neighbour choices)",
    "B2x1": "FULL decision density 2x1", "B1x2": "FULL decision density 1x2",
    "B2x2": "FULL decision density 2x2",
    "C": "luma per pixel, chroma per 2x2 block",
    "D": "LIGHT samples only the nearer real frame",
    "E": "LIGHT tiles with sub-threshold motion are reused",
    "F": "dual stage: LIGHT first, FULL only where LIGHT residual is high",
    "G": "interleaved quality: odd intermediate frames run FULL tiles as LIGHT",
}


@dataclass
class Params:
    tile: int = 16                 # analysis/executor block size (8, 16 or 32)
    levels: int = 3                # coarsest pyramid level (1/8 resolution)
    coarse_range: int = 6          # full search radius at the coarsest level (level pixels)
    me_bias: float = 1.0 / 1024    # ME cost bias per level pixel of |v|_1 (keeps flat areas at zero)
    static_thr: float = 1.0 / 255  # E0 (mean zero-vector difference) below this -> REUSE eligible
    static_max_thr: float = 16.0 / 255  # M0 (max zero-vector difference) below this -> REUSE eligible
    light_thr: float = 3.0 / 255   # probe residual along the smooth field below this -> LIGHT eligible
    probe: int = 4                 # probe grid per tile side (probe^2 samples per tile)
    cut_thr: float = 20.0 / 255    # median E and E0 above this -> scene cut (hold)
    residual_thr: float = 6.0 / 255  # LIGHT tile mean |a-b| above this -> promote to FULL
    occ_lo: float = 0.04           # FULL: |a-b| where nearest-side fallback starts
    occ_hi: float = 0.12           # FULL: |a-b| where nearest-side fallback is complete
    smooth_bias: float = 0.01      # FULL: candidate cost bias per px of deviation from v_smooth
    hysteresis: int = 2            # consecutive eligible pairs before a tile is downgraded
    adaptive: bool = True          # False: every tile uses `force_class`
    force_class: int = CLASS_FULL
    full_density: int = 2          # FULL candidate decision per 1x1 or 2x2 pixel block
    e_motion_thr: float = 0.5      # experiment E: LIGHT tiles moving less than this (px) become REUSE

    def as_dict(self):
        return asdict(self)


C_HEADER_FIELDS = (  # name in Params -> macro in te_params.h
    ("tile", "TE_DEFAULT_TILE"),
    ("levels", "TE_DEFAULT_LEVELS"),
    ("coarse_range", "TE_DEFAULT_COARSE_RANGE"),
    ("me_bias", "TE_DEFAULT_ME_BIAS"),
    ("static_thr", "TE_DEFAULT_STATIC_THR"),
    ("static_max_thr", "TE_DEFAULT_STATIC_MAX_THR"),
    ("light_thr", "TE_DEFAULT_LIGHT_THR"),
    ("probe", "TE_DEFAULT_PROBE"),
    ("cut_thr", "TE_DEFAULT_CUT_THR"),
    ("residual_thr", "TE_DEFAULT_RESIDUAL_THR"),
    ("occ_lo", "TE_DEFAULT_OCC_LO"),
    ("occ_hi", "TE_DEFAULT_OCC_HI"),
    ("smooth_bias", "TE_DEFAULT_SMOOTH_BIAS"),
    ("hysteresis", "TE_DEFAULT_HYSTERESIS"),
    ("full_density", "TE_DEFAULT_FULL_DENSITY"),
)

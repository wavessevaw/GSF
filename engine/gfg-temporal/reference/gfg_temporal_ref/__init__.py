"""Reference implementation of the GFG Temporal Engine (executable specification)."""
from .params import CLASS_FULL, CLASS_LIGHT, CLASS_NAMES, CLASS_REUSE, EXPERIMENTS, Params
from .core import TemporalEngine, linear_blend, repeat_nearest

__all__ = ["CLASS_FULL", "CLASS_LIGHT", "CLASS_NAMES", "CLASS_REUSE", "EXPERIMENTS", "Params",
           "TemporalEngine", "linear_blend", "repeat_nearest"]

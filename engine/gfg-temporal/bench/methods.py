"""Generators compared on the bench.  Each takes real frames in order with
push(frame_uint8, timestamps) and returns the frames between the previous real
frame and this one (empty for the first frame)."""
from __future__ import annotations

import numpy as np

from gfg_temporal_ref import TemporalEngine


class _Pair:
    """Helper for stateless two-frame methods."""

    def __init__(self, w, h):
        self.prev = None

    def push(self, frame, ts):
        cur = frame.astype(np.float32) / 255.0
        prev, self.prev = self.prev, cur
        if prev is None:
            return []
        return [self.pair(prev, cur, t) for t in ts]


class Repeat(_Pair):
    def pair(self, a, b, t):
        return a if t < 0.5 else b


class Blend(_Pair):
    def pair(self, a, b, t):
        return a * (1 - t) + b * t


class TE2:
    """te.2 / te.3 algorithm via its executable reference (mirrors the shaders)."""

    def __init__(self, w, h):
        self.e = TemporalEngine()

    def push(self, frame, ts):
        outs, _ = self.e.push(frame, ts)
        return outs


REGISTRY = {"repeat": Repeat, "blend": Blend, "te2": TE2}


def register(name):
    def deco(cls):
        REGISTRY[name] = cls
        return cls
    return deco


def make(name, w, h):
    if name not in REGISTRY:
        # lazily import optional generators so a broken one never blocks the rest
        import importlib
        importlib.import_module(f"gen_{name}")
    return REGISTRY[name](w, h)

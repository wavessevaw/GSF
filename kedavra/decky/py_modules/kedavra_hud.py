# SPDX-License-Identifier: GPL-3.0-or-later
"""On-screen HUD: rings with the frame rate that reaches the screen.

The plugin renders a small BGRA bitmap once a second and publishes it atomically; the
KEDAVRA Gamescope draws it as a layer of every composited frame (kfg_hud). The numbers come
from the engine's status file, so they count real KMS page flips, generated frames included,
not what the game submits.

Rings (the look follows GFG Extreme's ring HUD: dark rounded panel, thin track, coloured arc,
number in the middle, label below):
  FPS   frames on screen per second; arc = share of the refresh rate; below: the game's own fps
  x2/x3 the current multiplier; arc = share of generated frames; label AUTO in auto mode
  PACE  share of vblanks that showed a new frame (not a repeat); green when above 99.5%

File: 8 x u32 header {magic 'KFGH', version 1, width, height, corner, margin, sequence, 0},
then width * height * 4 bytes BGRA; alpha 0 outside the panel. Width 0 hides the HUD.
"""
from __future__ import annotations

import base64
import json
import math
import os
import struct
import zlib
from functools import lru_cache
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

MAGIC = 0x4847464B  # "KFGH"
VERSION = 1
HEADER = struct.Struct("<8I")
CORNERS = {"top-left": 0, "top-right": 1, "bottom-left": 2, "bottom-right": 3}
DEFAULT_PATH = Path("/dev/shm/kedavra-hud.raw")

PANEL = (14, 14, 17)
TRACK = (255, 255, 255)
TRACK_ALPHA = 0.16
RED = (251, 13, 0)
GREEN = (46, 200, 110)
YELLOW = (230, 185, 40)
GREY = (92, 92, 102)
WHITE = (245, 245, 247)
LABEL = (205, 205, 212)

Rgb = Tuple[int, int, int]


@lru_cache(maxsize=1)
def _atlas() -> Dict[str, Any]:
    from kedavra_hud_font import DATA
    return json.loads(zlib.decompress(base64.b64decode("".join(DATA))))


@lru_cache(maxsize=1024)
def _coverage(encoded: str) -> bytes:
    return base64.b64decode(encoded)


def scale_for(height: int) -> float:
    """Atlas scale for the output height (800 p = 1)."""
    return min((1.0, 1.5, 2.0), key=lambda s: abs(s - max(1.0, height / 800.0)))


class Canvas:
    """BGRA pixels on an opaque rounded panel; alpha 0 outside it."""

    def __init__(self, w: int, h: int, radius: float) -> None:
        self.w, self.h = w, h
        self.px = bytearray(_panel(w, h, radius))

    def blend(self, x: int, y: int, rgb: Rgb, a: float) -> None:
        if a <= 0 or not (0 <= x < self.w and 0 <= y < self.h):
            return
        i = (y * self.w + x) * 4
        if self.px[i + 3] == 0:
            return
        a = min(1.0, a)
        for k, c in ((0, rgb[2]), (1, rgb[1]), (2, rgb[0])):
            self.px[i + k] = round(self.px[i + k] + (c - self.px[i + k]) * a)

    def ring(self, cx: float, cy: float, size: float, width: float, frac: float, rgb: Rgb) -> None:
        """Track plus an arc clockwise from 12 o'clock with round caps."""
        r = (size - width) / 2.0
        half = width / 2.0
        frac = max(0.0, min(1.0, frac))
        end = frac * 2 * math.pi
        caps = [(0.0, -r), (r * math.sin(end), -r * math.cos(end))] if frac > 0 else []
        for y in range(math.floor(cy - size / 2) - 1, math.ceil(cy + size / 2) + 2):
            for x in range(math.floor(cx - size / 2) - 1, math.ceil(cx + size / 2) + 2):
                dx, dy = x + 0.5 - cx, y + 0.5 - cy
                band = max(0.0, min(1.0, half + 0.5 - abs(math.hypot(dx, dy) - r)))
                if band <= 0:
                    continue
                self.blend(x, y, TRACK, band * TRACK_ALPHA)
                if frac <= 0:
                    continue
                if frac >= 1:
                    arc = band
                else:
                    angle = math.atan2(dx, -dy) % (2 * math.pi)
                    arc = band * max(0.0, min(1.0, (end - angle) * r + 0.5))
                    for px, py in caps:
                        arc = max(arc, max(0.0, min(1.0, half + 0.5 - math.hypot(dx - px, dy - py))))
                self.blend(x, y, rgb, arc)

    def text_width(self, text: str, style: str) -> float:
        font = _atlas()[style]
        space = font["glyphs"][" "]
        return (sum(font["glyphs"].get(ch, space)["adv"] for ch in text)
                + font["spacing"] * max(0, len(text) - 1))

    def text(self, cx: float, top: float, text: str, style: str, rgb: Rgb) -> None:
        """Centred on cx; top is the top of the line box."""
        font = _atlas()[style]
        space = font["glyphs"][" "]
        x = cx - self.text_width(text, style) / 2.0
        for ch in text:
            g = font["glyphs"].get(ch, space)
            cov = _coverage(g["a"])
            gx, gy = round(x + g["x0"]), round(top + g["y0"])
            for row in range(g["h"]):
                for col in range(g["w"]):
                    a = cov[row * g["w"] + col]
                    if a:
                        self.blend(gx + col, gy + row, rgb, a / 255.0)
            x += g["adv"] + font["spacing"]


@lru_cache(maxsize=8)
def _panel(w: int, h: int, radius: float) -> bytes:
    px = bytearray(w * h * 4)
    color = bytes((PANEL[2], PANEL[1], PANEL[0], 255))
    for y in range(h):
        for x in range(w):
            cx = min(max(x + 0.5, radius), w - radius)
            cy = min(max(y + 0.5, radius), h - radius)
            if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= radius * radius:
                px[(y * w + x) * 4:(y * w + x) * 4 + 4] = color
    return bytes(px)


def _num(v: Any) -> Optional[float]:
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or v < 0:
        return None
    return float(v)


class Rates:
    """Per-second rates from consecutive engine status snapshots (cumulative counters)."""

    KEYS = ("flips", "sources", "real", "gen", "repeat")

    def __init__(self) -> None:
        self.prev: Optional[Dict[str, Any]] = None

    def update(self, st: Dict[str, Any]) -> Optional[Dict[str, float]]:
        t = _num(st.get("time_ns"))
        if t is None:
            return None
        prev, self.prev = self.prev, st
        if prev is None or _num(prev.get("time_ns")) is None:
            return None
        dt = (t - prev["time_ns"]) / 1e9
        if not 0.2 <= dt <= 5.0:
            return None
        out = {}
        for k in self.KEYS:
            a, b = _num(prev.get(k)), _num(st.get(k))
            if a is None or b is None or b < a:
                return None
            out[k] = (b - a) / dt
        return out


def items_for(st: Dict[str, Any], rates: Optional[Dict[str, float]]) -> List[Dict[str, Any]]:
    refresh = 1e9 / st["refresh_ns"] if _num(st.get("refresh_ns")) else 90.0
    active = st.get("state") == "active"
    fps = rates["flips"] if rates else None
    real = rates["sources"] if rates else None
    shown = (rates["real"] + rates["gen"] + rates["repeat"]) if rates else 0.0
    items = [{
        "size": 52, "w": 4.5, "frac": (fps or 0) / refresh, "rgb": RED,
        "text": str(round(fps)) if fps is not None else "—", "style": "fps",
        "sub": f"{round(real)} REAL" if real is not None else None, "label": "FPS"}]
    mult = st.get("multiplier") if isinstance(st.get("multiplier"), int) else 1
    gen_share = rates["gen"] / shown if rates and shown > 0 else 0.0
    items.append({
        "size": 46, "w": 4, "frac": gen_share if active else 0.0, "rgb": GREEN if active else GREY,
        "text": f"x{mult}" if active and mult > 1 else "OFF", "style": "val",
        "label": "AUTO" if st.get("auto") else "GEN"})
    if active and rates and shown > 0:
        new = 1.0 - rates["repeat"] / shown
        rgb = GREEN if new >= 0.995 else (YELLOW if new >= 0.97 else RED)
        items.append({"size": 46, "w": 4, "frac": new, "rgb": rgb,
                      "text": f"{min(99.9, 100 * new):.1f}" if new < 0.9995 else "100", "style": "val",
                      "label": "PACE"})
    return items


def render(st: Dict[str, Any], rates: Optional[Dict[str, float]], scale: float = 1.0) -> Tuple[int, int, bytes]:
    s = scale
    items = items_for(st, rates)
    pad_x, pad_y, gap, col, lab_h = 8 * s, 6 * s, 8 * s, 52 * s, 11 * s
    widths = [it["size"] * s for it in items]
    w = math.ceil(2 * pad_x + sum(widths) + gap * (len(items) - 1))
    h = math.ceil(2 * pad_y + col + 2 * s + lab_h)
    cv = Canvas(w, h, 12 * s)
    atlas = _atlas()
    x = pad_x
    for it, iw in zip(items, widths):
        cx, cy = x + iw / 2.0, pad_y + col / 2.0
        cv.ring(cx, cy, it["size"] * s, it["w"] * s, it["frac"], it["rgb"])
        style = f"{it['style']}@{s}"
        sub_style = f"sub@{s}"
        th = atlas[style]["ascent"]
        sub = it.get("sub")
        total = th + (atlas[sub_style]["ascent"] + s if sub else 0)
        top = cy - total / 2.0
        cv.text(cx, top, it["text"], style, WHITE)
        if sub:
            cv.text(cx, top + th + s, sub, sub_style, LABEL)
        cv.text(cx, pad_y + col + 2 * s, it["label"], f"lab@{s}", LABEL)
        x += iw + gap
    return w, h, bytes(cv.px)


def publish(path: Path, w: int, h: int, pixels: bytes, *, corner: str = "top-left",
            margin: int = 12, seq: int = 0) -> bool:
    """Atomic write; readable by the Gamescope user. w = 0 hides the HUD."""
    header = HEADER.pack(MAGIC, VERSION, w, h, CORNERS.get(corner, 0), margin, seq & 0xFFFFFFFF, 0)
    tmp = path.with_name(f".{path.name}.{os.getpid()}")
    try:
        with open(tmp, "wb") as f:
            f.write(header)
            f.write(pixels)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
        return True
    except OSError:
        try:
            tmp.unlink()
        except OSError:
            pass
        return False


class Hud:
    """Called once a second by the plugin with the engine status (or None)."""

    def __init__(self, path: Path = DEFAULT_PATH) -> None:
        self.path = path
        self.rates = Rates()
        self.seq = 0
        self.last_key: Optional[str] = None
        self.shown = False

    def tick(self, enabled: bool, status: Optional[Dict[str, Any]]) -> bool:
        fresh = bool(status) and status.get("engine") == "running"
        if not enabled or not fresh:
            self.rates.prev = None
            if self.shown or self.last_key is None:
                self.shown = False
                self.last_key = ""
                return publish(self.path, 0, 0, b"", seq=self.seq)
            return True
        rates = self.rates.update(status)
        scale = scale_for(int(status.get("height") or 800))
        items = items_for(status, rates)
        key = json.dumps([scale, items], sort_keys=True, default=str)
        self.seq += 1
        if key == self.last_key:
            # Same picture: refresh the file time only, so Gamescope keeps showing it.
            try:
                os.utime(self.path)
                return True
            except OSError:
                pass
        w, h, px = render(status, rates, scale)
        self.last_key = key
        self.shown = True
        return publish(self.path, w, h, px, margin=round(12 * scale), seq=self.seq)

# SPDX-License-Identifier: GPL-3.0-or-later
"""On-screen HUD: rings with the frame rate that reaches the screen.

The plugin renders a small BGRA bitmap once a second and publishes it atomically; the
KEDAVRA Gamescope draws it as a layer of every composited frame (kfg_hud). The numbers come
from the engine's status file, so they count real KMS page flips, generated frames included,
not what the game submits.

Rings (the look follows GFG Extreme's ring HUD, in the KEDAVRA green: dark rounded panel with
the logo, thin track, coloured arc, number in the middle, label below):
  FPS   new frames on screen per second (real + generated; with generation off, the game's own
        frames). Not the KMS flip rate: Gamescope also flips for the Steam menu and overlays,
        which made the ring read 90 while the game ran at 25. Below: the game's own fps.
  x2/x3 the current multiplier; arc = share of generated frames; label AUTO in auto mode, CAP
        and the base cap with a fixed base frame rate (x2.3 for 40 at 90 Hz)
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

PANEL = (6, 9, 7)
TRACK = (255, 255, 255)
TRACK_ALPHA = 0.14
RED = (255, 90, 74)
GREEN = (82, 214, 74)
YELLOW = (227, 192, 74)
GREY = (111, 125, 117)
WHITE = (242, 246, 243)
LABEL = (207, 214, 209)

Rgb = Tuple[int, int, int]


@lru_cache(maxsize=1)
def _atlas() -> Dict[str, Any]:
    from kedavra_hud_font import DATA
    return json.loads(zlib.decompress(base64.b64decode("".join(DATA))))


@lru_cache(maxsize=4)
def _logo(scale: float) -> Optional[Tuple[int, bytes]]:
    try:
        from kedavra_hud_logo import LOGO
        px, data = LOGO[f"{scale:.1f}"]
        return px, zlib.decompress(base64.b64decode(data))
    except (ImportError, KeyError, ValueError, zlib.error):
        return None


@lru_cache(maxsize=1024)
def _coverage(encoded: str) -> bytes:
    return base64.b64decode(encoded)


def scale_for(height: int) -> float:
    """Atlas scale for the output height (800 p = 1; a 1080p TV 1.5, a 4K TV 3)."""
    return min((1.0, 1.5, 2.0, 3.0), key=lambda s: abs(s - max(1.0, height / 800.0)))


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

    def image(self, x0: int, y0: int, size: int, rgba: bytes) -> None:
        for y in range(size):
            for x in range(size):
                i = (y * size + x) * 4
                a = rgba[i + 3]
                if a:
                    self.blend(x0 + x, y0 + y, (rgba[i], rgba[i + 1], rgba[i + 2]), a / 255.0)

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
    OPTIONAL = ("unsupported", "degrade")

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
        for k in self.OPTIONAL:
            a, b = _num(prev.get(k)), _num(st.get(k))
            out[k] = (b - a) / dt if a is not None and b is not None and b >= a else 0.0
        return out


def mult_text(v: float) -> str:
    """x2.25, x3.75 exactly (quarter steps); x2.6 for 35 at 90 Hz."""
    if abs(v * 4 - round(v * 4)) < 0.01:
        return "x" + f"{round(v * 4) / 4:.2f}".rstrip("0").rstrip(".")
    return f"x{v:.1f}"


def _rate_color(v: float, target: float) -> Rgb:
    return GREEN if v >= 0.97 * target else (YELLOW if v >= 0.85 * target else RED)


def live_for(st: Dict[str, Any], rates: Optional[Dict[str, float]]) -> Dict[str, Any]:
    """What the HUD and the panel show: fps on screen, the game's fps, pace, generated share."""
    active = st.get("state") == "active"
    if not rates or st.get("steam_ui") is True:
        # No rates yet, or Steam's own UI is on screen: its repaints are not game frames.
        return {"fps": None, "real": None, "pace": None, "gen_share": None, "unsupported": 0.0}
    real = rates["sources"]
    shown = rates["real"] + rates["gen"] + rates["repeat"]
    # New frames per second, never more than the flips that carried them.
    fps = min(rates["flips"], rates["real"] + rates["gen"]) if active else min(rates["flips"], real)
    return {"fps": fps, "real": real,
            "pace": 1.0 - rates["repeat"] / shown if active and shown > 0 else None,
            "gen_share": rates["gen"] / shown if active and shown > 0 else 0.0,
            # Paints per second whose game frame generation could not take (format, HDR).
            "unsupported": rates.get("unsupported", 0.0)}


def items_for(st: Dict[str, Any], rates: Optional[Dict[str, float]]) -> List[Dict[str, Any]]:
    refresh = 1e9 / st["refresh_ns"] if _num(st.get("refresh_ns")) else 90.0
    active = st.get("state") == "active"
    lv = live_for(st, rates)
    real, fps = lv["real"], lv["fps"]
    shown = (rates["real"] + rates["gen"] + rates["repeat"]) if rates else 0.0
    base = st.get("base_fps") if isinstance(st.get("base_fps"), int) else 0
    items = [{
        "size": 52, "w": 4.5, "frac": (fps or 0) / refresh,
        "rgb": _rate_color(fps or 0, refresh) if active else GREEN,
        "text": str(round(fps)) if fps is not None else "—", "style": "fps",
        "sub": f"{round(real)} REAL" if real is not None else None, "label": "FPS"}]
    mult = st.get("multiplier") if isinstance(st.get("multiplier"), int) else 1
    gen_share = rates["gen"] / shown if rates and shown > 0 else 0.0
    # Auto picks its cap too (30, 35, 40 or 45 at 90 Hz): x2.6 at 35, not the scheduler's 3.
    cap = st.get("cap_fps") if isinstance(st.get("cap_fps"), int) else 0
    eff = base or (cap if st.get("auto") else 0)
    if active and eff and round(refresh) % eff:
        text = mult_text(round(refresh) / eff)
    else:
        text = f"x{mult}" if active and mult > 1 else "OFF"
    label = f"CAP {base}" if base else ("AUTO" if st.get("auto") else "GEN")
    items.append({
        "size": 46, "w": 4, "frac": gen_share if active else 0.0, "rgb": GREEN if active else GREY,
        "text": text, "style": "val", "label": label})
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
    logo = _logo(s)
    lw = logo[0] + gap if logo else 0
    w = math.ceil(2 * pad_x + lw + sum(widths) + gap * (len(items) - 1))
    h = math.ceil(2 * pad_y + col + 2 * s + lab_h)
    cv = Canvas(w, h, 12 * s)
    atlas = _atlas()
    x = pad_x
    if logo:
        cv.image(round(pad_x), round(pad_y + (col - logo[0]) / 2), logo[0], logo[1])
        x += lw
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
        self.live: Dict[str, Any] = live_for({}, None)

    def tick(self, enabled: bool, status: Optional[Dict[str, Any]]) -> bool:
        fresh = bool(status) and status.get("engine") == "running"
        rates = self.rates.update(status) if fresh else None
        if not fresh:
            self.rates.prev = None
        self.live = live_for(status or {}, rates)
        if not enabled or not fresh:
            if self.shown or self.last_key is None:
                self.shown = False
                self.last_key = ""
                return publish(self.path, 0, 0, b"", seq=self.seq)
            return True
        # The screen's height, not the game's: a 1080p game on a 4K TV gets a 4K-sized HUD.
        scale = scale_for(int(status.get("output_height") or status.get("height") or 800))
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

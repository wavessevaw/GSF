# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "py_modules"))
import kedavra_hud  # noqa: E402


def status(t, flips, sources, real, gen, repeat, **kw):
    st = {"engine": "running", "state": "active", "refresh_ns": 11111111, "multiplier": 3,
          "auto": True, "height": 800, "time_ns": t, "flips": flips, "sources": sources,
          "real": real, "gen": gen, "repeat": repeat}
    st.update(kw)
    return st


class HudTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.path = self.tmp / "hud.raw"
        self.hud = kedavra_hud.Hud(self.path)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def header(self):
        return struct.unpack("<8I", self.path.read_bytes()[:32])

    def test_rates_count_flips_not_game_frames(self):
        r = kedavra_hud.Rates()
        self.assertIsNone(r.update(status(1_000_000_000, 90, 30, 30, 60, 0)))
        rates = r.update(status(2_000_000_000, 180, 60, 60, 120, 0))
        self.assertEqual(rates["flips"], 90.0)
        self.assertEqual(rates["sources"], 30.0)
        items = kedavra_hud.items_for(status(0, 0, 0, 0, 0, 0), rates)
        self.assertEqual(items[0]["text"], "90")
        self.assertEqual(items[0]["sub"], "30 REAL")
        self.assertEqual(items[1]["text"], "x3")
        self.assertEqual(items[1]["label"], "AUTO")

    def test_off_shows_game_frames_not_flips(self):
        # Gamescope flips at the refresh rate for the Steam menu alone; with generation off
        # the ring must show the game's 25 fps, not 90.
        r = kedavra_hud.Rates()
        off = dict(state="off", auto=False, multiplier=1)
        r.update(status(1_000_000_000, 90, 25, 0, 0, 0, **off))
        rates = r.update(status(2_000_000_000, 180, 50, 0, 0, 0, **off))
        items = kedavra_hud.items_for(status(0, 0, 0, 0, 0, 0, **off), rates)
        self.assertEqual(items[0]["text"], "25")
        self.assertEqual(items[0]["sub"], "25 REAL")
        self.assertEqual(items[1]["text"], "OFF")

    def test_active_counts_new_frames_not_repeats(self):
        r = kedavra_hud.Rates()
        r.update(status(1_000_000_000, 90, 30, 30, 60, 0))
        rates = r.update(status(2_000_000_000, 180, 60, 55, 110, 15))
        items = kedavra_hud.items_for(status(0, 0, 0, 0, 0, 0), rates)
        self.assertEqual(items[0]["text"], "75")
        self.assertEqual(items[2]["label"], "PACE")

    def test_steam_ui_is_not_game_frames(self):
        # The Steam client as the base plane repaints at the refresh rate, uncapped: no game
        # FPS is shown for it.
        lv = kedavra_hud.live_for(status(0, 0, 0, 0, 0, 0, steam_ui=True),
                                  {"flips": 90.0, "sources": 90.0, "real": 0.0, "gen": 0.0, "repeat": 0.0})
        self.assertIsNone(lv["real"])
        self.assertIsNone(lv["fps"])

    def test_base_cap_label(self):
        items = kedavra_hud.items_for(status(0, 0, 0, 0, 0, 0, base_fps=40, auto=False), None)
        self.assertEqual(items[1]["label"], "CAP 40")
        self.assertEqual(items[1]["text"], "x2.25")
        items = kedavra_hud.items_for(status(0, 0, 0, 0, 0, 0, base_fps=24, auto=False), None)
        self.assertEqual(items[1]["text"], "x3.75")

    def test_auto_fractional_cap(self):
        # Auto at 35 fps on 90 Hz fills x2.6 (the scheduler's multiplier says 3).
        items = kedavra_hud.items_for(status(0, 0, 0, 0, 0, 0, cap_fps=35), None)
        self.assertEqual(items[1]["label"], "AUTO")
        self.assertEqual(items[1]["text"], "x2.6")
        items = kedavra_hud.items_for(status(0, 0, 0, 0, 0, 0, cap_fps=30), None)
        self.assertEqual(items[1]["text"], "x3")

    def test_publishes_bitmap_and_hides(self):
        self.hud.tick(True, status(1_000_000_000, 90, 30, 30, 60, 0))
        self.hud.tick(True, status(2_000_000_000, 180, 60, 60, 120, 0))
        magic, ver, w, h, corner, margin, seq, _ = self.header()
        self.assertEqual((magic, ver), (kedavra_hud.MAGIC, 1))
        self.assertGreater(w, 100)
        self.assertEqual(len(self.path.read_bytes()), 32 + w * h * 4)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o644)
        self.hud.tick(False, None)
        self.assertEqual(self.header()[2:4], (0, 0))

    def test_scale_follows_the_screen_not_the_game(self):
        self.assertEqual(kedavra_hud.scale_for(800), 1.0)
        self.assertEqual(kedavra_hud.scale_for(1080), 1.5)
        self.assertEqual(kedavra_hud.scale_for(2160), 3.0)
        self.hud.tick(True, status(1_000_000_000, 90, 30, 30, 60, 0))
        self.hud.tick(True, status(2_000_000_000, 180, 60, 60, 120, 0))
        w800 = self.header()[2]
        # A 1080p game on a 4K TV: the HUD is sized for 2160 lines.
        self.hud.tick(True, status(3_000_000_000, 270, 90, 90, 180, 0, height=1080, output_height=2160))
        self.assertGreater(self.header()[2], 2.5 * w800)

    def test_stale_engine_hides(self):
        self.hud.tick(True, status(1_000_000_000, 90, 30, 30, 60, 0))
        self.hud.tick(True, {"engine": "stale"})
        self.assertEqual(self.header()[2:4], (0, 0))

    def test_corners_are_transparent_panel_is_opaque(self):
        w, h, px = kedavra_hud.render(status(0, 0, 0, 0, 0, 0), None)
        self.assertEqual(px[3], 0)                                  # rounded corner
        mid = ((h // 2) * w + 2) * 4
        self.assertEqual(px[mid + 3], 255)


if __name__ == "__main__":
    unittest.main()

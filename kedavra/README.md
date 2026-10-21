# KEDAVRA (GSF 1.1.2 Kedavra)

Independent x2/x3 frame generation for Steam Deck OLED in Gaming Mode, generated and
presented by a patched Gamescope, controlled from Decky. No Lossless Scaling.

| Path | What |
|---|---|
| `sched/` | vblank-indexed scheduler (`kfg_sched.h`), unit and fuzz tests, compositor model `kfg_sim`, stress matrix |
| `shaders/`, `engine/` | interpolation shaders, shared pass plan, standalone Vulkan host and correctness/benchmark test |
| `tools/kfg_pacing.py` | pacing analyzer for model and KMS-flip traces |
| `gamescope/` | Gamescope integration (module + hooks) |
| `deck/` | probe, bench, reversible install, rollback |
| `decky/` | Decky plugin |

Docs: `docs/kedavra/` (phase A history, phase B architecture, phase C evidence, Deck protocol).

```sh
make -C kedavra test     # scheduler tests, model stress matrix, engine on a Vulkan device
make -C kedavra bench    # engine GPU timing (meaningful on the Deck)
```

Pacing targets are not yet confirmed by a KMS flip trace on Steam Deck hardware for 1.1.2; see `docs/kedavra/DECK_PROTOCOL.md` and the project [README](../README.md).

## Control (gamescopectl)

| Convar | Values |
|---|---|
| `kfg_multiplier` | 0 off, 1 Auto, 2 = x2, 3 = x3 |
| `kfg_base_fps` | base frame cap before generation, 20 and up (at 90 Hz from 24, x3.75); 0 = from the multiplier |
| `kfg_auto_floor` | lowest cap Auto may use: 0 = refresh / 3 (x3), down to refresh / 3.75 (24 at 90 Hz, x3.75); echoed as `req_auto_floor`, the floor in force as `auto_floor` |
| `kfg_post` | filters on the frame each vblank shows, after generation, comma separated: `deband`, `hdr_look`, `clarity`, `cartoon`, `levels_plus`, `curves`, `vibrance`, `colourfulness`, `technicolor2`, `dpx`, `technicolor`, `bleach_bypass`, `sepia`, `noir`, `monochrome`, `nostalgia`, `chromatic_aberration`, `vignette`, `film_grain`, `cas=S`, `dls=S:D`, `fxaa`, `smaa` (S sharpness, D denoise, 0..1). Empty = off. They run with generation on or off, on SDR game frames only; the HUD and Steam overlays stay unfiltered. |
| `kfg_policy` | revision number a controller sets after the others; echoed as `policy` in the status file with `req_multiplier` and `req_base_fps`, so the controller can confirm its whole policy arrived |
| `kfg_trace`, `kfg_status`, `kfg_hud`, `kfg_budget_us`, `kfg_debug_marker` | trace file, status file, HUD bitmap, GPU budget per composite (generation + filters), generated-frame marker |

Filter sources and licences: `shaders/THIRD_PARTY.md`. Filter tests (CPU references for
the colour looks, flat-image and edge checks for sharpening and antialiasing):
`build/kfg_posttest`; `--bench 50` times the plugin presets on the device it runs on.

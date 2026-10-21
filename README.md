<div align="center">

**English** · [Русский](README.ru.md)

<img src="docs/img/logo.png" alt="GSF Kedavra" width="144">

# GSF Kedavra

**Independent x2 / x3 frame generation for Steam Deck in Gaming Mode. No Lossless Scaling.**

GSF (GPU Synthetic Frames) generates the in-between frames inside a patched Gamescope, the compositor that already owns every vblank and every page flip on the Deck. A Decky plugin installs it, switches it on and off and measures the result.

[**Download 1.0.0**](https://github.com/wavessevaw/GSF/releases/tag/v1.0.0) · [Install](#installation) · [Modes](#modes) · [Measuring pacing](#measuring-pacing) · [How it works](#how-it-works)

[![Release](https://img.shields.io/github/v/release/wavessevaw/GSF?style=flat-square&color=52d64a&label=release)](https://github.com/wavessevaw/GSF/releases/latest)
[![Steam Deck](https://img.shields.io/badge/Steam%20Deck-OLED-0d1210?style=flat-square)](https://www.steamdeck.com/)
[![Decky](https://img.shields.io/badge/Decky%20Loader-plugin-0d1210?style=flat-square)](https://decky.xyz/)
[![SDR](https://img.shields.io/badge/output-SDR-0d1210?style=flat-square)](#known-limits)
[![License](https://img.shields.io/badge/license-GPL--3.0-0d1210?style=flat-square)](LICENSE.md)

<img src="docs/img/interface.png" alt="GSF Kedavra interface: the Decky panel with a 90 FPS ring at a 30 FPS cap, the in-game HUD, the Pace and Setup views" width="900">

<sub>The plugin's own interface code and HUD renderer, rendered with sample values. Not a measurement.</sub>

</div>

> **Status: experimental.** The engine, scheduler and plugin pass their software tests in CI, and the build runs on a Steam Deck OLED. The pacing targets below have **not yet been confirmed by a KMS flip trace on hardware** for 1.0.0. Earlier builds measured on the Deck did not meet them; see [the Deck evidence](docs/kedavra/evidence/deck-20261021/README.md).

## Why GSF Kedavra

- **No Lossless Scaling.** No paid app, no DLL from another program, no Vulkan layer inside the game. GSF has its own interpolation engine (GPL-3.0).
- **Works on every game Gamescope shows.** Frames are taken from the compositor, so there is nothing to inject and no per-game launch option.
- **One zip, nothing by hand.** The Decky plugin installs and updates its own Gamescope build, checks the system and rolls back by itself. No Desktop Mode scripts.
- **The system image stays untouched.** The KEDAVRA Gamescope lives in `/var/lib/kedavra` and is mounted over the stock one only while the service runs. A boot that does not reach Gaming Mode returns to the stock Gamescope on the next boot.
- **Frame pacing comes first.** A vblank-indexed scheduler decides what every refresh shows, and the game is held on a fixed cadence that generation can fill evenly.
- **No FPS limit to set by hand.** In **Auto** the engine caps the game itself and picks x2 or x3 from what the game really delivers. Or set your own base FPS with one slider.
- **Honest numbers.** The ring counts new frames on screen and the game's own frames, and the pacing record is taken from KMS page-flip timestamps, not from an FPS counter.

## Installation

Requires [Decky Loader](https://decky.xyz/). Tested on SteamOS 3.8.

1. Download [**GSF-Kedavra-1.0.0.zip**](https://github.com/wavessevaw/GSF/releases/download/v1.0.0/GSF-Kedavra-1.0.0.zip).
2. In Decky: **Settings → Developer → Install plugin from zip**, pick the file. Upgrading over an older KEDAVRA plugin keeps your settings.
3. Open the plugin, go to **Setup** and press **Install Gamescope KEDAVRA**. Reboot when asked.
4. After the reboot every check in **Setup** is green, with no line saying the installed Gamescope differs from the one in the plugin.

To remove it, press **Remove and restore stock Gamescope** in **Setup**, then reboot.

## Modes

| Mode | What it does |
| --- | --- |
| **Off** | The game runs untouched. The ring shows the game's own frame rate. |
| **Auto** | The engine limits the game to 30 FPS (x3) or 45 FPS (x2) at 90 Hz. It starts at x3, tries x2 after a clean stretch and returns to x3 as soon as the game cannot keep 45. |
| **Custom FPS** | The engine holds the game at the base FPS you set (20 to 90) and generation fills the rest of the refresh. Rates that divide the refresh (30, 45 at 90 Hz) are the smoothest. Others, like 40, work with a 2-2-3 cadence. |

**For the most stable result, lock the frame rate in the game's own settings** (30 or 45) and choose the same value as **Custom FPS**. A game that paces its own frames gives the generator evenly spaced inputs. No Steam FPS limit is needed: GSF sets the limit itself while generation is on and gives Steam's back when it is off.

Generation needs at least a few milliseconds of GPU time per game frame for motion analysis. If the game already uses all of the GPU, lower the base FPS.

## In-game HUD

<img src="docs/img/hud.png" alt="In-game HUD: 90 FPS on screen, 30 real, x3, pace 100" width="320">

The optional HUD draws three rings over the game, in the same style as [GFG Extreme](https://github.com/wavessevaw/GFG-Extreme):

- **FPS:** new frames that reach the screen each second (real and generated). Below it, **REAL** is the game's own frame rate. Repeated frames and Steam menu repaints are not counted.
- **x2 / x3:** the current multiplier. The arc is the share of generated frames. The label says **AUTO** or **CAP** with the base FPS.
- **PACE:** the share of refreshes that showed a new frame. Green above 99.5%.

## Measuring pacing

**Pace → Record pacing trace** writes the KMS flip time of every refresh while you play. Stop it after one or two minutes and the plugin analyses the trace on the device. The 1.0.0 targets are:

| Metric | Target |
| --- | --- |
| P95 interval error | ≤ 1 ms |
| P99 interval error | ≤ 2 ms |
| Missed refreshes | < 0.5% |
| Oscillation, latency drift | none |

Save the diagnostics report (**Setup → Report**) and attach it with the trace when you report a problem.

## How it works

1. Gamescope copies every new game frame into a small GPU ring.
2. Right after the frame is composited, a separate submission runs hierarchical motion estimation between it and the previous frame, split over two refreshes so it never delays a flip.
3. On every vblank the scheduler (`kedavra/sched/kfg_sched.h`) picks a real frame or an intermediate at the right phase. Frames from a game whose presents jitter around the cap keep their place on the cap grid, so motion stays even.
4. The intermediate is synthesised at full resolution with occlusion-aware blending, and Gamescope flips it like any other frame.
5. The plugin reads the engine status once a second, draws the HUD and records traces.

More detail: [architecture](docs/kedavra/PHASE_B_ARCHITECTURE.md), [Deck test protocol](docs/kedavra/DECK_PROTOCOL.md), [engine and scheduler sources](kedavra/README.md).

## Known limits

- SDR output only. HDR games pass through without generation.
- Built and tested for Steam Deck OLED at 90 Hz. Other refresh rates work in the model and tests but have not been measured on hardware.
- Fine repeating detail (stairs, fences, grilles) is the hardest case for any motion estimator. 1.0.0 reduces the shimmer it caused, but it can still show at very low base FPS.

## Building from source

```sh
make -C kedavra test                 # scheduler tests, model stress matrix, engine on a Vulkan device (lavapipe works)
make -C kedavra bench                # engine GPU timing (meaningful on the Deck)
sh kedavra/decky/package.sh out.zip  # plugin zip without the Gamescope runtime
```

The full plugin with the Gamescope runtime is built by [CI](.github/workflows/kedavra.yml) against Gamescope `aa289ea`.

## Repository

- [`kedavra/`](kedavra/): the current engine, scheduler, Gamescope integration and Decky plugin.
- [`engine/`](engine/), [`decky/`](decky/), [`docs/fg3/`](docs/fg3/), [`docs/archive/`](docs/archive/README.md): earlier GSF research (Vulkan layer prototypes, Temporal Engine, audits), kept for history.
- GSF is part of the same family as [GFG Extreme](https://github.com/wavessevaw/GFG-Extreme). See [MIGRATION.md](MIGRATION.md) for how the code was separated.

## License

**GPL-3.0-or-later**, with upstream copyright and third-party notices preserved: [license](LICENSE.md), [copyright](COPYRIGHT.md), [third-party notices](NOTICE.md), [licensing guidance in Russian](docs/LICENSING.ru.md). The GPL does not grant rights to present modified builds as official GSF releases; see [brand-use guidelines](TRADEMARKS.md). Contributions: [CONTRIBUTING.md](CONTRIBUTING.md).

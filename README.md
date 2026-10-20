# GSF Next 0.2.0 Alpha 5

Experimental efficiency update following Alpha 4. The default reconstructs into
private GPU output images, removing one scratch image and one full-image copy per
synthetic. Shared motion analysis, bounded buffering, ×2/×3/×4, absolute pacing
and live profile controls remain available. An opt-in batch shader shares flow
sampling and HUD detection across two/three outputs.

- [Installable Alpha 5 ZIP](GSF-Next-0.2.0-alpha.5.zip)
- [SHA-256](GSF-Next-0.2.0-alpha.5.zip.sha256)
- [Source](https://github.com/wavessevaw/GSF/tree/ee61c4213b58570d5b99c1f1bbb73a3a4a8d2d0d)
- [Installation](https://github.com/wavessevaw/GSF/blob/ee61c4213b58570d5b99c1f1bbb73a3a4a8d2d0d/decky-next/README.md)
- [Implementation and compatibility](https://github.com/wavessevaw/GSF/blob/ee61c4213b58570d5b99c1f1bbb73a3a4a8d2d0d/docs/gsf-next/ALPHA5_EFFICIENCY.md)
- [Measured software comparison and limitations](https://github.com/wavessevaw/GSF/blob/ee61c4213b58570d5b99c1f1bbb73a3a4a8d2d0d/docs/gsf-next/EFFICIENCY_REPORT.md)
- [Pull request](https://github.com/wavessevaw/GSF/pull/55)

SHA-256: `3232c2f8726ea8f29b3a4a0476ae2d338ecb77994d2f7885526e047a0601f7fe`

The ZIP includes corresponding sources, attribution/licenses, an offline native
payload, hardware test protocol and 35 checked internal file hashes. It passed
25 actual Vulkan/WSI/pixel/lifetime cases, direct/batched live-control tests,
Decky backend/panel checks and the extracted production launcher ×2/×3/×4.

The eliminated logical copy traffic is structural. CPU Vulkan timings **do not
show a uniform default-path speedup**. The batch path reduced reconstruction cost
on the software workload but stays opt-in pending RDNA2 measurements. A failed
single-worker 1280×800 run is explicitly retained as failed, without weakening
admission/sample thresholds. Physical cadence, source game FPS, acceptable game
image quality, input latency and 10/15-W power are not qualified. No LSFG/MAKO
parity or achieved 45-to-90 hardware claim is made. Stable/main is untouched.

Restart the game once after updating. Select its actual display refresh. To check
45-to-90, allow 90 at the Steam/Gamescope output cap and limit source FPS inside
the game or DXVK. Ordinary settings and multipliers can subsequently change live;
experimental copy/batch diagnostic overrides require restart and are documented.

Previous experimental builds remain available for rollback:
[Alpha 4](GSF-Next-0.2.0-alpha.4.zip),
[Alpha 2](GSF-Next-0.2.0-alpha.2.zip),
[Alpha 1](GSF-Next-0.2.0-alpha.1.zip).

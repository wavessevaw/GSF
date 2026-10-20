# GSF Next 0.2.0 Alpha 3

Experimental Decky plugin with private GPU synthetic storage, late-sequence
compaction, absolute pacing and source-priority admission before GPU work.

- [Installable Alpha 3 ZIP](GSF-Next-0.2.0-alpha.3.zip)
- [SHA-256](GSF-Next-0.2.0-alpha.3.zip.sha256)
- [Source](https://github.com/wavessevaw/GSF/tree/9e528d650f7bd52a0a2ff48b46b757b5336d2f14)
- [Installation](https://github.com/wavessevaw/GSF/blob/9e528d650f7bd52a0a2ff48b46b757b5336d2f14/decky-next/README.md)
- [Architecture, evidence and limitations](https://github.com/wavessevaw/GSF/blob/9e528d650f7bd52a0a2ff48b46b757b5336d2f14/docs/gsf-next/ALPHA3_OUTPUT.md)
- [Pull request](https://github.com/wavessevaw/GSF/pull/54)

SHA-256: `7d8e523f56ddd707769ba334c26c262a1ccbc196bade323b44c13f6ec0113c2e`

The package includes corresponding source, licenses, offline native payload and
hardware acceptance instructions. Actual Vulkan WSI/pixel/ownership tests, live
controls, Decky tests and the extracted launcher x2/x3/x4 were checked locally.
The software cadence correctness clock uses 30 Hz with explicitly scaled source
intervals; deterministic 90-Hz clock tests are separate. CPU-only real-time 90-Hz
throughput is not consistently achieved. Physical Deck output, native FPS and
power require a new hardware run. No LSFG/MAKO parity or achieved 45-to-90 hardware
claim is made. Stable/main is untouched.

Restart the game once after updating. Select the actual display refresh. For a
45-to-90 check, let the Steam/Gamescope output cap pass 90, and cap source FPS
inside the game or DXVK. Settings and multipliers can subsequently change live.

Previous experimental builds remain available for rollback:
[Alpha 2](GSF-Next-0.2.0-alpha.2.zip), [Alpha 1](GSF-Next-0.2.0-alpha.1.zip).

# GSF Next 0.2.0 Alpha 4

Experimental Decky plugin with private GPU synthetic storage, late-sequence
compaction, absolute pacing, source-priority admission and isolated Gamescope WSI.
The Gamescope compositor and Gaming Mode stay active; only the game-process
WSI shim is disabled.

- [Installable Alpha 4 ZIP](GSF-Next-0.2.0-alpha.4.zip)
- [SHA-256](GSF-Next-0.2.0-alpha.4.zip.sha256)
- [Source](https://github.com/wavessevaw/GSF/tree/c92ea2919a9bccdca46ea7f14180be4e7e3bc0a1)
- [Installation](https://github.com/wavessevaw/GSF/blob/c92ea2919a9bccdca46ea7f14180be4e7e3bc0a1/decky-next/README.md)
- [Architecture, evidence and limitations](https://github.com/wavessevaw/GSF/blob/c92ea2919a9bccdca46ea7f14180be4e7e3bc0a1/docs/gsf-next/ALPHA4_OUTPUT.md)
- [Pull request](https://github.com/wavessevaw/GSF/pull/54)

SHA-256: `92b76d5940b29c639c7c4b9d53a96bfb5c448d338b4c2b705107d67b85c4e103`

The package includes corresponding source, licenses, offline native payload and
hardware acceptance instructions. Actual Vulkan WSI/pixel/ownership tests, live
controls, Decky tests and the extracted launcher x2/x3/x4 were checked locally.
The software cadence metrics use 30 Hz with explicitly scaled source intervals;
failed numeric throughput acceptance stays recorded as failed. Deterministic
90-Hz clock and full Vulkan pixel/ownership tests are separate. CPU-only real-time 90-Hz
throughput is not consistently achieved. Physical Deck output, native FPS and
power require a new hardware run. No LSFG/MAKO parity or achieved 45-to-90 hardware
claim is made. Stable/main is untouched.

Restart the game once after updating. Select the actual display refresh. For a
45-to-90 check, let the Steam/Gamescope output cap pass 90, and cap source FPS
inside the game or DXVK. Settings and multipliers can subsequently change live.

Previous experimental builds remain available for rollback:
[Alpha 2](GSF-Next-0.2.0-alpha.2.zip), [Alpha 1](GSF-Next-0.2.0-alpha.1.zip).

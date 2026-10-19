# GSF — GPU Synthetic Frames

Independent research and development of a Vulkan GPU frame-generation engine for Linux and SteamOS.

**Status: EXPERIMENTAL.** This repository is being separated from [GFG Extreme](https://github.com/wavessevaw/GFG-Extreme). Software validation and a real Vulkan WSI prototype exist, but **Steam Deck OLED physical display output, stable 2×/3×, latency and HDR are not accepted**. Do not use as a production-ready gaming feature.

[Game-generation blocker audit, 2026-10-19](docs/audit/20261019/GENERATION_BLOCKERS_RU.md): six hardware captures, scoped GPU deadline evidence, packed10 activation refusal, and a reproducible historical dispatch regression. Passing the audit tests does not qualify a Deck candidate.

## GSF Next: separate 0.2.0 Alpha

[GSF Next Decky plugin](decky-next/README.md) replaces the previous experimental
runtime with an attributed open AFMF-Linux / FidelityFX Optical Flow fork, shared
×2/×3/×4 synthesis and asynchronous presentation. It does not load Flow Lite or
Lossless Scaling components. This is a new **unqualified Alpha**, not a stable
upgrade or evidence of LSFG/MAKO-level game compatibility. See its
[architecture](docs/gsf-next/ARCHITECTURE.md),
[software results](docs/gsf-next/SOFTWARE_REPORT.md) and
[Deck acceptance protocol](docs/gsf-next/DECK_PROTOCOL.ru.md).

## Components

- [Native Vulkan layer and output experiments](engine/gfg-native/): real-WSI interception, GPU bridge, present scheduling, Vulkan loader integration and unit / lavapipe tests.
- [Temporal Engine and GPU LMC](engine/gfg-temporal/): Vulkan compute and interpolation algorithms, shaders, reference tests and benchmarks.
- [Historical GSF investigation](docs/fg3/): architecture decisions, failures, validation and hardware acceptance requirements.
- [Recovered pre-migration research](docs/archive/README.md): pinned MAKO/LSFG audits, host-recorder evidence and alternative Output Engine documents from GFG branches.
- [AFMF-Linux comparison](docs/GSF_AFMF_LINUX_SOURCE_AUDIT_20261018.md): reference source audit, WSI and RDNA2 risks.

See [MIGRATION.md](MIGRATION.md) for source provenance, history access and limitations.

## Current development priorities

The current Decky test baseline is [0.1.7](decky/README.md). Independent
builds, strict 5 Hz GPU pixel correctness, real Vulkan WSI and extracted-package
tests run in CI. These software results do not qualify variable-rate Deck 2×.

1. Validate calibrated submit-to-GPU-start measurements and calibration overhead with comparable workloads; keep unproven pure queue wait null.
2. Protect source frames and use actual deadline/slack and measured GPU cost for admission, without a fixed source-FPS floor.
3. Preserve image lifetime, semaphore consumption, ordered WSI handoffs and recovery through scene/format/swapchain changes.
4. Qualify sustained SDR 2× under variable game FPS on Deck, including source frametimes, latency and evidence of unique displayed frames.
5. Address HDR10 and 3× after SDR 2× acceptance. Keep historical cold-start failures distinct from warmed software correctness.

## License, attribution and official identity

**GSF is GPL-3.0-or-later**, with original upstream copyright and third-party notices preserved. See [full license](LICENSE.md), [copyright provenance](COPYRIGHT.md), [third-party inventory](NOTICE.md), and [licensing guidance in Russian](docs/LICENSING.ru.md). Under the GPL you may run, study, modify and redistribute this code, including commercially, if you comply with its conditions. This license does **not** grant rights to impersonate official GSF releases or make false endorsements; see [brand-use guidelines](TRADEMARKS.md). Nothing here claims registered trademark ownership or alters the GPL.

For contributions, follow [CONTRIBUTING.md](CONTRIBUTING.md). The pre-extraction commit history remains in GFG Extreme; see [MIGRATION.md](MIGRATION.md).

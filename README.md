# GSF — GPU Synthetic Frames

Independent research and development of a Vulkan GPU frame-generation engine for Linux and SteamOS.

**Status: EXPERIMENTAL.** This repository is being separated from [GFG Extreme](https://github.com/wavessevaw/GFG-Extreme). Software validation and a real Vulkan WSI prototype exist, but **Steam Deck OLED physical display output, stable 2×/3×, latency and HDR are not accepted**. Do not use as a production-ready gaming feature.

## Components

- [Native Vulkan layer and output experiments](engine/gfg-native/): real-WSI interception, GPU bridge, present scheduling, Vulkan loader integration and unit / lavapipe tests.
- [Temporal Engine and GPU LMC](engine/gfg-temporal/): Vulkan compute and interpolation algorithms, shaders, reference tests and benchmarks.
- [Historical GSF investigation](docs/fg3/): architecture decisions, failures, validation and hardware acceptance requirements.
- [AFMF-Linux comparison](docs/GSF_AFMF_LINUX_SOURCE_AUDIT_20261018.md): reference source audit, WSI and RDNA2 risks.

See [MIGRATION.md](MIGRATION.md) for source provenance, history access and limitations.

## First development priorities

1. Reproduce the inherited Native FG3 software validation failure (8 real / 8 synthetic / 6 skipped / 0 verified checks).
2. Validate Vulkan ownership/synchronization and **actual intermediate pixels**, without weakening tests.
3. Make independent native + temporal builds reproducible.
4. Prove ordered GSF 2× output in a controlled Vulkan WSI test.
5. Qualify Deck SDR2× on hardware; only then HDR10 and 3×.

## License

Extracted source inherits its applicable upstream terms. See [LICENSE.md](LICENSE.md) and the original GFG Extreme licensing statements. Do not assume this extraction relicenses MAKO-derived portions to MIT.

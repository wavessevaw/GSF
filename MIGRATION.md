# GSF repository extraction and provenance

**Extraction date:** 2026-10-18. **Source:** [wavessevaw/GFG-Extreme](https://github.com/wavessevaw/GFG-Extreme). **Source snapshot:** [1b600fbc4e6a03a9616fd65ee2f5b16f5224da00](https://github.com/wavessevaw/GFG-Extreme/tree/1b600fbc4e6a03a9616fd65ee2f5b16f5224da00), the GSF research continuation branch based on Direct Real WSI #155. This snapshot includes the AFMF source audit from #158.

## What moved

234 source files from the pinned snapshot were imported into an independent GitHub repository; their blob SHA-1 values were programmatically compared against the source Git tree and **all 234 matched exactly**. This covers:
- `engine/gfg-native/`: complete native Vulkan Layer, GPU bridge, WSI/output experiments, packaging and tests.
- `engine/gfg-temporal/`: complete Temporal Engine / GPU LMC, shaders, references, benchmark scripts and tests.
- `engine/gfg-fusion/`: experimental Frame Fusion renderer patch and integration test.
- `docs/fg3/`: text research, acceptance plans, checkpoint/evidence logs and JSON data, excluding historical ZIP binaries.
- `docs/temporal_bench_results/` and related `docs/GSF_*`, `docs/TEMPORAL_ENGINE*`, `docs/FRAME_FUSION.md`.
- The specific GSF build/diagnostic scripts in `scripts/` and original GSF/Temporal workflows.
- The original `LICENSE.md`.

Workflows tied to the **full GFG Extreme Decky release or frontend** were preserved under `.github/legacy-workflows/` rather than executed. Independently runnable native, WSI and Temporal Engine workflows remain in `.github/workflows/`.

## Historical Git ancestry caveat

**This is a verified snapshot extraction, NOT a true `git-filter-repo` history rewrite.** The GitHub integration used for migration could not clone/push Git history or create a filtered commit graph. New GSF commits start a **new history**; original author/date/blame information for work done before extraction remains in GFG Extreme, at the exact source revision linked above. Do **not** claim `git log` in this repository exposes every earlier original commit.

Relevant original pull requests and their discussions:
- [#132 issue contract](https://github.com/wavessevaw/GFG-Extreme/issues/132), [#133 native integration](https://github.com/wavessevaw/GFG-Extreme/pull/133).
- [#144–150 experimental Gaming Mode labs and pacing](https://github.com/wavessevaw/GFG-Extreme/pulls?q=is%3Apr+GSF+created%3A2026-10-17).
- [#151 loader physical-device dispatcher fix](https://github.com/wavessevaw/GFG-Extreme/pull/151).
- [#153 output owner](https://github.com/wavessevaw/GFG-Extreme/pull/153).
- [#154 offscreen source pool and virtual swapchain](https://github.com/wavessevaw/GFG-Extreme/pull/154).
- [#155 direct real WSI](https://github.com/wavessevaw/GFG-Extreme/pull/155).
- [#158 AFMF-Linux research](https://github.com/wavessevaw/GFG-Extreme/pull/158).

To later migrate **actual historical ancestry** use a fresh local clone and `git-filter-repo` on explicitly selected path histories; first reconcile with all commits made in GSF after this extraction. Do not force-push public `main` automatically or overwrite collaborators' changes.

## Intentionally NOT included

- Binary ZIP artifacts and evidence archives from `docs/fg3/artifacts` / `docs/fg3/evidence`. Their provenance and SHA256SUMS were retained in text; see [HISTORIC_ARCHIVES.md](docs/HISTORIC_ARCHIVES.md). Archive binaries remain available in the original source repository.
- GFG Extreme's Decky frontend, Governor, Autopilot, user config, stable releases and MAKO packaging. These remain maintained in GFG Extreme, which should eventually consume **versioned** GSF binaries / API rather than embedding another independent copy of the engine.
- Runtime validation on Steam Deck. CI and source equivalence do not prove physical OLED 2×/3× output.

## Immediate post-extraction work

1. Verify standalone source builds and all GSF-relevant native/Temporal tests in the new repository; keep inherited Native FG3 LMC GPU CI failure visible.
2. Port GFG-coupled packaging and scripts into native standalone GSF CLI with explicit dependencies and checksums.
3. Establish a versioned interface for GFG Extreme; do not remove its existing integrated generator until a functional replacement is accepted.
4. Qualify Game Mode output on real Steam Deck hardware with actual frame-sequence evidence before shipping or claiming HDR/3×.
5. Keep license/copyright attribution under GPL-3.0-or-later and all third-party notices where required.

# Review of Argon 4 / Qwen: GSF Motion Intelligence

**Date:** 2026-10-18. **Status:** independently checked research review; **NOT** an accepted new runtime architecture. **Input:** Argon 4's user-supplied nine-part "GSF Motion Intelligence" report. This document captures **the corrected conclusions, conflicts with current GSF code and a falsifiable development plan**, not a verbatim third-party report. **Scope:** SteamOS / Steam Deck RDNA2, GSF independently of Lossless Scaling.

## Executive decision

**Accept the strategic principle:** native game motion vectors (MV) and depth are potentially useful but never guaranteed from arbitrary Vulkan / DXVK / VKD3D-Proton titles. Keep a **game-agnostic color-frame interpolation base** and only add validated external information under a fail-safe, opt-in contract.

**Reject rebuilding a three-level LMC replacement.** Existing GSF already contains block/pyramid correspondence, projective camera models, motion palette, HUD history/masking, residual/orphan handling and full-resolution synthesis. Argon's proposal is largely **already implemented**, and its numerous quantitative percentages, performance promises and "100% quality" claims had no measurements or citations.

**Priority order:** (0) finish independent GSF native WSI 45 real → 90 actually visible SDR2×; (1) stabilize cold/warm GPU LMC tests; (2) offline A/B improvement of present model/HUD/occlusion; (3) isolated resource-intelligence prototype with strong provenance and strict opt-in.

## 1. Source-verified comparison: what already exists

| Argon proposal | Actual GSF component | Interpretation |
| --- | --- | --- |
| Multiscale block-match optical flow | `engine/gfg-temporal/shaders/lmc/lmc_search.comp`, `lmc_down.comp`, `lmc_field.comp` | Already exists, not a novel architectural proposal |
| Camera-motion estimation and projective layers | `engine/gfg-temporal/src/te_lmc_fit.c` (RANSAC + robust IRLS projective homographies), `lmc_layers.comp`, `lmc_irls.comp` | Implemented in both CPU-reference and GPU LMC variants |
| Moving-object motion palette | `te_lmc_fit.c`, `lmc_palette.comp` | Already exists |
| HUD segmentation and protection | `lmc_layers.comp`, `lmc_masked.comp`, `lmc_compose.comp` | `hudCount` history mask protects detected HUD; `lmc_compose.comp` copies nearest real frame for HUD pixels |
| Disocclusion / unreliable correspondence | `lmc_field.comp`, `lmc_masked.comp`, `lmc_orphan.comp`, `lmc_compose.comp` | Already searches residual/orphan regions and uses one-sided sampling; **not** an actual physical depth buffer |
| Temporal history and scene-cut handling | `te_lmc.inc`, `te_engine.h`, `lmc_sf_prepare.comp` | Existing temporal motion state; **not** a guarantee of stored T−2 full-color frame |
| 2x/3x output generation | `te_engine.h` `TE_MAX_OUTPUTS=4`, arbitrary increasing intermediate timestamps | Implemented computation, but no Deck optical WSI 2x/3x hardware acceptance |
| Optional engine MV/depth inputs | No general engine resource contract currently exposed in public `te_context_desc` | **Potentially new**, significant compatibility/performance risk |
| Standalone HUD confidence metrics and UI-specific benchmark | `docs/fg3/SHARED_FLOW_PROTOTYPE.md` already records HUD ROI and a 2dB HUD regression in one previous experiment | Extend this evaluation rather than claiming HUD is solved |

**Explicit existing benchmark caveat:** the prior Shared Flow investigation recorded HUD quality regressions under some compute optimizations, so a new HUD classifier requires per-ROI **non-regression**, not merely a pleasant-looking screenshot.

## 2. Factual corrections to the external report

### 2.1 Depth clip control ≠ compression

`VK_EXT_depth_clip_control` controls OpenGL-style normalized device depth range `[-1,1]` vs Vulkan `[0,1]`; **it does not compress depth resources**. Reverse-Z is a depth mapping choice; AMD depth compression/HTILE is a separate implementation-level topic. A surface whose Vulkan format is `R32_SFLOAT` is not necessarily depth at all.

Source: [official Vulkan extension specification](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_depth_clip_control.html).

### 2.2 A queue submit does not expose the recorded command stream

`vkQueueSubmit` receives **opaque command-buffer handles and synchronization metadata**. A graphics-aware Vulkan layer must proactively intercept `vkCmdBeginRenderPass` / `vkCmdBeginRendering`, descriptor updates/bindings, render target lifetimes, `vkCmdExecuteCommands`, reset/free operations, `vkQueueSubmit2`, and relevant extensions **at command recording and resource-creation time**, or use driver-level capture tooling. It cannot simply "rewind all vkCmdDraw calls" after seeing `vkQueueSubmit`.

`VK_EXT_descriptor_buffer` permits GPU-side descriptor updates and bypasses older `vkCmdBindDescriptorSets` handling; thus tracking only `vkCmdBindDescriptorSets` misses real workloads. Also account for bindless heaps, aliasing, transient resources, dynamic rendering, separate queue families, device loss and games that never create dedicated velocity textures.

Sources: [Vulkan command buffer semantics](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html), [descriptor buffers](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_buffer.html).

### 2.3 "True game motion vectors" are not "100% accurate output"

MV candidates need validation of: coordinate space (UV vs pixels), vector direction/sign, projection/jitter and resolution scale, TAA history alignment, motion-blur/transparent object coverage, reverse Z, disocclusion, shader effect distortion, correct frame pair ID and scene cut. Wrong-but-numerically-plausible vectors can be worse than optical flow. Merely clamping vector magnitudes is **insufficient**. Native MV ≠ perfect FG.

The official [AMD FidelityFX frame generation API](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/frame-interpolation-api.md) has separate resources for depth/MV, UI without HUD, distortion fields and different interpolation stages, illustrating the additional contracts required.

### 2.4 No evidence for universal percentages and millisecond promises

The original report provided **no traceable benchmark or URL** for `30% games pack RG vectors`, `60% detection`, `70–85% quality`, `1–2 ms optical flow`, `<2.5 ms on Deck`, `3–5 ms frame-generation budget`, or `>10ms neural inference`. These are **hypotheses / proposed experiment thresholds**, not facts.

At 45→90, nominal **display interval is 11.11ms**, game real period **22.22ms**. No fixed "remaining 5–7ms" follows: the real GPU and frame generator compete for the same 8-CU APU, variable driver + Gamescope composition + queue time + memory budget. Async compute can improve overlap when resources allow but is **not guaranteed to run for free**.

At 30→90, **two** synthetic frames must fit an effective 33.33ms real-source cadence and the required display deadlines, while input latency/quality degrade with lower real base FPS. AMD's integration [guidance for classic FSR3](https://gpuopen.com/fidelityfx-super-resolution-3/) recommends a higher pre-interpolation frame rate; current [FSR FG guidelines](https://gpuopen.com/amd-fsr-framegeneration/) emphasize artifact and pacing risk at low source rates. This does **not** prohibit GSF 30→90 experimentation.

### 2.5 Steam Deck memory bandwidth was misstated

Official [Steam Deck OLED technical specifications](https://www.steamdeck.com/en/tech/oled/) give LPDDR5 **6400MT/s quad 32-bit channels** = **128-bit bus** = theoretical 102.4GB/s aggregate bandwidth. The Argon statement 47–64GB/s is not the OLED hardware's theoretical figure. The usable rate is smaller and must be measured; CPU/GPU share it.

"All compute must be in-place or aliased" is **not a universal safe requirement**: interpolation requires source references and destination images with non-overlapping image lifetimes and correct Vulkan access/layout barriers. GPU-local resident transfers, bounded scratch reuse and appropriate transient memory aliasing can help when legal; unsafe in-place aliasing corrupts source imagery.

### 2.6 HUD and T−2 are weak hypotheses, not universal solutions

A high-contrast, zero-flow feature might be a stationary HUD **or** a static scene/foreground, while moving HUD/animations/transparency can have motion. The current GSF history-based HUD mask is imperfect; add confidence+temporal stability and measure false positives, not a universal Sobel threshold.

T−2 can recover a **previously visible** region only with valid reprojection, temporal ordering and low ghosting; the T−2 color must actually be retained, and this can increase VRAM and GPU traffic. It cannot reconstruct never-observed geometry, and a three-frame stored history **does not inherently impose one extra frame of latency** if all input frames are already in the past; actual future-frame dependence is the larger temporal FG latency question.

Neural depth estimation is a low-priority Deck experiment because of likely throughput cost, but it is **not logically impossible** without comparative benchmark data. Even a predicted depth map cannot recover arbitrarily missing scene geometry.

### 2.7 DXVK and VKD3D paths are not zero-cost universal hooks

Tracking Vulkan image provenance requires resource metadata, descriptor translation, Vulkan extension support and synchronization across app frames; a candidate `R16G16_SFLOAT` image is not a reliable velocity surface. DXVK/VKD3D state transitions do **not** semantically label a motion-vector texture. False candidates can cause artifacts; **unsafe access/rewrite can crash or corrupt applications** even if candidate selection is accurate.

The user's "no modifications to games" restriction does not imply anti-cheat compatibility: injecting a Vulkan layer or changing translation modules can be blocked by anti-cheat. No attempt should be made to bypass anti-cheat or protected game memory; restrict early prototypes to permitted test apps and offline captures.

## 3. Design decision: GSF Motion Intelligence as optional evidence, not a rewrite

```text
Real frames A/B
   ├─ Existing GSF LMC: pyramid motion, 2-sided candidates,
   │    camera projective models, palette, residual/orphan field,
   │    HUD count / scene cut
   ├─ [Optional, disabled] Provenance-Audited Game MV / Depth adapter
   │     • negotiated format/layout/timestamp + game/source ID
   │     • confirmed producer and lifetime, no guessed pNext
   │     • normalized coordinate convention
   │     • confidence vs independent existing LMC optical estimate
   │     • reject unexpected color/motion/temporal inconsistency
   └─ [Future offline A/B] HUD/disocclusion confidence field
          ↓ confidence-gated arbitration and real-only fallback
     Existing LMC synthesis and Output Worker
          ↓
     Gamescope WSI → physical scanout (not assumed)
```

**Tier A (default):** current GSF GPU LMC only. Quality and cadence against unchanged baseline.

**Tier B (experiment):** add **offline** per-pixel confidence diagnostics to motion/HUD/occlusion using **already computed** LMC vectors and residuals; no new game interception, no additional first-frame dispatch until benchmarks validate it.

**Tier C (optional, later):** game-MV/depth attachment from a trusted integration contract (controlled test application, documented engine callback, explicit XR-like producer) before investing in generic Vulkan heuristics. Every imported source is selected **by provenance**, not only a format/size guess; confidence threshold must be able to reject it, including deliberate adversarial plausible data. Generic command-stream tracker only if opt-in benchmark shows value in enough games to justify hot-path overhead.

**Do not replace GSF LMC with untested FFX sources.** Open-source SDK licensing must be verified at the pinned upstream revision, and benchmarked against GSF's independent, GPL-licensed architecture. The official AMD FSR3 package may be MIT under some components, but the presence of a published SDK alone does not grant permission to copy unrelated drivers, shader assets or vendor DLLs.

## 4. Small falsifiable research experiments, independent of Deck WSI delivery

| Priority | Experiment | Success / disqualification |
| --- | --- | --- |
| P0 | Preserve baseline: compare exact GSF LMC vs any new proposal on held-out static camera, fast camera, HUD animation, thin objects, crosses, smoke/water and scene cuts | Baseline PSNR/SSIM, HUD ROI temporal shimmer, edge ghosting, GPU stage timings and no regression within predeclared tolerances |
| P1 | Offline MV adapter using an intentionally tagged test scene with **ground-truth** forward/back motion and optional depth | Correct sign, pixel/UV scaling, reprojected frame IDs, occluded pixels rejected; intentional corrupted MV falls back to pure LMC |
| P1 | API candidate discovery **metadata-only** on a standalone Vulkan/DXVK test app, no texture reads, no rewriting submissions | Candidate recall/precision and CPU overhead measured; no leaks/deadlocks, mixed extensions and secondary CB support, real frame unaffected |
| P1 | HUD confidence using current `hudCount` and per-pixel residual | Improves HUD ROI with no unacceptable false positives or flicker on stationary high-contrast scene |
| P2 | T−2 temporal color cache / reprojected hole fill | Measured improvement only on synthetic occlusion scenes; bounded VRAM, GPU time and no ghosting regression; do not enable if negative |
| P2 | Real RDNA2 GPU time, power and 45→90 quality | Show end-to-end per-stage timing on a Deck and genuine physical display after existing WSI path reaches hardware acceptance |

**Useful metrics:** count accepted/abandoned MV candidates, blind quality delta per ROI against GSF LMC, source/synthetic checksum/ordering, temporal self-consistency, GPU budget and shared memory, frame drops, input-to-display latency. Failure must mean **return to GSF's existing LMC**, then real-only when LMC cannot meet deadline. No hidden blacklists or automatic injection into online protected games.

## 5. Recommendation to development and to Argon 4

1. **Do not pause GSF Output/WSI** for a new optical-flow subsystem. Our primary gating milestone is **45 real → 90 physically distinct SDR frames** on a Steam Deck OLED, which is not proven yet.
2. Before implementing a single new GPU shader, develop a **comparative matrix using existing GSF tests and reports** so we know which content is actually bad and why.
3. Ask Argon for a **corrected cited addendum** with 3 areas only: (a) forward/back MV normalization and depth/occlusion semantics (real API feature contracts and concrete source references); (b) Vulkan command-stream tracking complexity incl. descriptor buffers, barriers, dynamic rendering; (c) falsifiable GPU-cost/quality A/B plan on RDNA2 **without numerical claims presented as facts**.
4. The design goal is **better quality per watt without destabilizing Gamescope**, not replacing GSF with a second implementation of work already done.
5. A separate diagnostic branch can prototype **opt-in offline metadata-only** tracking once WSI pipeline's real framebuffer output and strict GPU LMC regression are under control.

## Sources

- Existing source: [GSF LMC implementation](https://github.com/wavessevaw/GSF/tree/main/engine/gfg-temporal), [historical shared-flow quality investigations](https://github.com/wavessevaw/GSF/blob/main/docs/fg3/SHARED_FLOW_PROTOTYPE.md).
- [Vulkan: command buffers](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html).
- [Vulkan: descriptor buffers](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_buffer.html).
- [Vulkan: depth clip control](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_depth_clip_control.html).
- [AMD FSR 3 GPUOpen guide](https://gpuopen.com/fidelityfx-super-resolution-3/).
- [AMD frame generation preparation, HUD and distortion-field contract](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/frame-interpolation-api.md).
- [Valve official Steam Deck OLED specifications](https://www.steamdeck.com/en/tech/oled/).
- Previous GSF audits: `docs/research/GSF_MESA_RADV_WSI_DEEP_AUDIT_20261018.md`, `docs/research/GSF_GAMESCOPE_PRESENT_TIMING_20261018.md`.

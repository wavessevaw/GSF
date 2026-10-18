# GSF strategic review: Qwen's six questions (temporal control and predictive FG)

**Date:** 2026-10-18. **Source:** Qwen strategic report supplied by the project owner, discussing predictive FG, output ownership, variable computation, 3× latency, motion history and quality control. **Status:** source-grounded architectural decision and mathematical test. **No Steam Deck performance claim or implementation.**
**Source code baseline:** [GSF main 64809aaa](https://github.com/wavessevaw/GSF/tree/64809aaa171698b1fe151e7e2f0b43b42f5f388a).

## Executive decision

Retain Qwen's fundamental premise: **GSF cannot interpolate a future-dependent pair before the second REAL image arrives; its visible output requires an actual, supported Gamescope-to-display path and it must account for visual confidence/latency.** Reject two misleading implementation assumptions:

1. It is not universally correct that a 30→90 interpolation path *must* add one whole 33.33ms source period of latency: a steady-state, evenly phased ideal pipeline admits lower bounds of **22.22ms plus first-synthetic readiness margin** at 30→90 and **11.11ms plus margin** at 45→90. These are not measured input-to-photon latency.
2. A private WSI proxy does **not** control physical scanout; it controls its own submit and presentation attempts. Gamescope, Vulkan WSI and DRM/KMS remain independent and must be measured.

Before new predictive shaders: finish **P0 real 45→90 SDR OLED output**, then quantify bounded delay/aborted generation, and only then compare causal/predictive extrapolation against buffered interpolation.

## 1. Important correction: the temporal lower bound, not a fixed one-frame number

Let REAL frames A_i and B=A_(i+1) become ready at times 0 and **T** relative to the pair. Let display interval be δ=T/m, with multiplier m, and assume **fixed output phase delay D** relative to the real source's availability (an ideal display with matching m× refresh). Synthetic j between A_i and B_i is scheduled at D+jδ, and needs real B plus GPU/render/transport readiness G_j.

Causality requires for **every** interpolated synthetic:
  
    D + jδ ≥ T + G_j   for j=1..m−1.

Therefore:

    D_min = max(0, max_j [T + G_j − jδ]).

If all intermediates require the same readiness G after B, the first j=1 is limiting:

    D_min = (m−1)δ + G = T−T/m + G.

This is a *thought-experiment lower bound*, not a real-world Deck target. Include compositor pre-present margin, GPU contention, capture time, variable FPS, buffers, queue waits and vsync quantization. If the first B takes 3ms to finish G1, idealized 30→90 lower bound is ~25.22ms, not 33.33ms + 3ms. For 45→90 it is ~14.11ms.

**Timeline demo at G=0 and 30→90:**

    t=0      REAL A ready (not necessarily displayed yet)
    t=22.22  display REAL A
    t=33.33  REAL B ready and display SYNTH A→B at τ=1/3
    t=44.44  display SYNTH A→B at τ=2/3
    t=55.56  display REAL B

This is self-consistent at zero compute cost; with nonzero compute and display margin, shift the lattice later or reduce generated output. A simple implementation **choosing to wait and display A only after B is available** adds ≥T, but that is an algorithmic policy, not an information-theoretic lower bound for all buffered schedules.

**Necessary scheduling distinction:** to reach D<T, the owner must show A *before B is available*, reserving future intermediate slots speculatively and abandoning synthetic if B is late. Existing `gfg_owner_capture` deliberately holds A until B arrives and schedules the first real ticket on capture of B, so its current behavior can indeed incur a full source-frame delay. A lower phase-shift bound does not mean this current owner achieves it. Test both the early-A/late-B dropout policy and the original wait-for-B owner strategy; real-frame timing must remain bounded.

**Latency accounting:** D is a **frame-availability-to-display output shift** under this abstract schedule. **Input-to-photon latency** additionally depends on when game input was sampled, simulation, CPU queueing, source render duration, compositor/DRM and screen scanout; no exact user-perceived added latency can be claimed from D alone. Three displayed images do not mean three new game simulation/input updates.

**For 3× with unequal readiness:** G_2 may be larger than G_1; test both deadlines. If G1=3ms, G2=20ms at 30→90, j=2 would require D≥31.11ms and dominate. Pipeline/profile first and second synthetic separately.

The Python math contract [test_gsf_temporal_causality.py](../../engine/gfg-native/tests/test_gsf_temporal_causality.py) verifies schedule bounds and negative impossible schedules. It proves **only arithmetic**, not GSF runtime timing or physical output.

## 2. Current GSF implementations: do not build them twice

| Qwen concept | Actual GSF code today | What remains missing |
| --- | --- | --- |
| Output Scheduler / frame clock | [frame_clock.h](../../engine/gfg-native/src/frame_clock.h): real-output grid, synthetic slots and 2×/3× test | This is **WSI handoff planning**, not confirmed compositor/scanout timing |
| Single visible presenter (owner) | [output_owner.h](../../engine/gfg-native/src/output_owner.h): ref scheduling, priorities, real-only fallback and bounded latency | Prototype **only**, has at most **one** synthetic ticket per captured real pair even on 90Hz |
| Virtual game WSI | [virtual_wsi_facade.h](../../engine/gfg-native/src/virtual_wsi_facade.h), virtual source and GPU copy retirement | Test path, not universal app-facing interception; cannot treat virtual VkSwapchainKHR as actual ICD WSI |
| Bounded async output | [output_worker.c](../../engine/gfg-native/src/output_worker.c): capacity one running + one queued; stale jobs, semaphore consumption | Stale and backlog in real WSI; no verified Deck-owned output chain |
| Application-device generation | [gpu_bridge.c](../../engine/gfg-native/src/gpu_bridge.c): real capture, TE generation, synthetic handoff | Application-visible WSI success ≠ physical presentation; possible compositor drops |
| Prior motion models | [te_lmc.inc](../../engine/gfg-temporal/src/te_lmc.inc): m->model_prev and m->have_temporal state | No full cross-frame reusable tile cache or validated predictive extrapolation |
| Adaptive work | [te_lmc.inc](../../engine/gfg-temporal/src/te_lmc.inc): lmc_tier and search-grid work reductions | Not yet proven to save energy with quality preserved on Deck |
| Per-output visibility | [lmc_select.comp](../../engine/gfg-temporal/shaders/lmc/lmc_select.comp), Shared Flow | Need independent confidence/quality proof (see Qwen disocclusion review #9) |

**Critical P0 discrepancy:** `frame_clock.h` admits **two** generated slots per 30→90 real pair in its isolated test, but `output_owner.h::gfg_owner_capture` emits only **one** synthetic ticket after B (plus real), even if the output refresh is 90Hz and the source is 30Hz. Existing owner tests cover **30→60**, not full 30→90 with two generated outputs. The owner transport must gain a separately tested 3× pair contract and per-output readiness/deadlines, without breaking one-copy of game render-finished semaphore.

The **private output owner is already proposed, not proven**. The source-of-truth design in [GSF_OUTPUT_OWNER_PROTOTYPE.md](../../docs/GSF_OUTPUT_OWNER_PROTOTYPE.md) explicitly says real-game hookup, Vulkan layout translation, renderer/view/barrier interception and Deck scanout remain open.

## 3. "Output owner" has several distinct meanings

1. **Image source ownership**: GSF can safely snapshot the game's VkImage without GPU readback, respecting render-finished semaphores, image layouts and source reuse.
2. **Submission/presentation request ownership**: one GSF module owns the visible WSI swapchain and submits ordered REAL/SYNTH requests with correct deadlines, present IDs and backpressure.
3. **Compositor scheduling**: Gamescope accepts and commits each selected request; may discard, replace or pace it.
4. **Hardware output/scanout**: DRM/KMS and panel observe distinct pixels on actual refresh cycles.

A Vulkan presentation proxy might prove (1)/(2) without proving (3)/(4). Only integrating with the compositor/DRM can give stronger *control*; even this requires validated hardware and compatible SteamOS. Do not conflate `vkQueuePresentKHR` success with physical pixels: [Khronos queue-present spec](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html). New Gamescope `VK_EXT_present_timing` may expose compositor feedback only when supported, but Gamescope first-pixel-out is **predicted**, not an optical sensor: [GSF timing audit](GSF_GAMESCOPE_PRESENT_TIMING_20261018.md).

**Next proof:** instrument one uniquely color/ID-marked real frame and two intermediates under controlled WSI, then prove correct app image retirement → actual output WSI acceptance → Gamescope compositor commit → physical OLED uniqueness. No changes to global Vulkan manifests, game anti-cheat or SteamOS compositor until a safe opt-in test exists.

## 4. Predictive frame generation is a new mode, not an established GSF engine

A future-freely extrapolated image is necessarily a prediction of motion and previously visible content. It can be viable for stable scenes and camera motion, but cannot know a new event/scene detail absent from history. **Input-aware late reprojection/timewarp is a distinct sub-problem:** it can use recent camera/input telemetry near scanout (if legitimately available), but also cannot infer unseen objects and is not a generic Vulkan-only guarantee.

A useful **controlled math/image prototype** before any GPU integration:
- Given recorded source A_(i−2), A_(i−1), A_i, predict image at A_i+δ and A_i+2δ using the already-fitted projective models and limited local vectors; never use future B, nor accidentally leak its optical ground truth.
- When B arrives, compute *error against the prediction*, score camera edges/foreground/scene cuts and keep a fail-safe fallback; **never claim ability to correct an already-presented synthetic image**.
- Compare with the same trace replayed as buffered interpolation at D_min (not artificially worse D=T), to avoid a biased comparison.
- Measure motion-compensated ROI quality, novel-object false positives, real input changes, frame age at presentation and p95/p99 present delay. If the prediction harms HUD or causes >threshold artifacts, disable.
- A repeated source frame is a **held frame**, not a generated new motion frame and should not count toward distinct display FPS.

## 5. Adaptive cost and cached motion should build on existing LMC, not replace it

Existing history (`m->model_prev`, `m->have_temporal`), variable search tier and time-dependent motion masks make a monolithic new GPU cache redundant. First check **how much GPU time** is spent on search, IRLS, blend, copies and synchronization on actual RADV.

**Minimal adaptive experiment:** static scene / low motion / fast pan / two moving objects / HUD, compare current tier=0/1/2 (where supported) and retained model age before adding tile lists. If full optical flow is already dominated by image moves or output WSI, adaptive search may not change p95 deadlines.

Tile-classify+indirect-dispatch is *not automatically faster*: classification, prefix/counts, barriers, indirect launch, LDS pressure and loss of coherence can exceed search savings on Steam Deck 8 CU. Only build sparse tiles after a measurable baseline and an independently trusted region classifier.

**Partial re-use cannot recover unseen objects**. Require age caps, occlusion/cut invalidation and a strict fallback to real or full compute when residual increases.

## 6. Quality/controller integration belongs *after* reliability evidence

GSF must not conflate **30 authentic render FPS** with 90 new gameplay simulation/input updates. Controller decisions should consider:
- source pacing, frame age, compositor confirmed/discarded, GPU generation cost, validated confidence and power;
- real frame priority with **bounded explicit interpolation delay** vs a low-latency mode with selective prediction or real-only fallback;
- reliability hysteresis/cooldowns, validated thresholds and **no spuriously claimed synthetic credits**.

A rule like "never sacrifice a real frame" means **no dropping, starving or allowing synthetic to cause unbounded real latency**. It does **not** mean real frames are always shown immediately: buffered interpolation intentionally delays them by a strictly bounded amount.

Controller paths are **future experimental policies**, not an automatic merge into GFG Extreme's Governor/Autopilot.

## 7. Required follow-up experiments and gates

| Priority | Task | Exit criterion |
| --- | --- | --- |
| P0 | Native real-game virtual WSI + single real-owner chain | A real Vulkan game captures frames through virtual images without stalling, correct semaphores, recreates and fallback |
| P1 (after 45→90 proof) | Add separate `output_owner` 30→90 **two-synthetic** reference ticket test, accounting for individual readiness `G1/G2` | 30 real IDs → 30 real IDs + 58 distinct synthetic tickets after warmup, ordered, no real drop or >bounded deadline |
| P0 | Hardware proof, actual Steam Deck Gamescope / OLED | 45 real→90 physically distinct SDR frames, safe resume/recreate, genuine IDs and recorded scanout/optical evidence |
| P0 | Reproduce LMC warm-first-dispatch and restore strict CI | Pixel checks and no unexpected skipped pairs within clearly declared warmed budget; cold-start failure classified separately |
| P1 | Mathematical causality and latency model (added) | 30→90 and 45→90 schedule inequalities pass, impossible too-early synth fails |
| P1 | Optical A/B predictive vs buffered interpolation | Same base 30/45 frame traces, no ground truth leakage; source age, scene-change recovery, input response and ROI fidelity |
| P1 | Profile existing LMC tier control and camera model caching | Measured RADV GPU/power wins without HUD/orphan regression |
| P2 | Optional tile adaptive search / output quality controller | No oscillation, bounded overhead and no fake FPS credits |

**Stop conditions:** no 3× or predictive feature shipping before source FPS vs panel FPS and true presentation evidence, no hidden anti-cheat injection, no rewriting Gamescope globally and no dropping GPU correctness gates.

## Source references

- [GSF output-owner prototype and limitations](../../docs/GSF_OUTPUT_OWNER_PROTOTYPE.md).
- [GSF Vulkan native source](../../engine/gfg-native/src/), including frame_clock, output_owner, output_worker, virtual WSI.
- [GSF GPU LMC implementation](../../engine/gfg-temporal/src/te_lmc.inc) and [LMC selection](../../engine/gfg-temporal/shaders/lmc/lmc_select.comp).
- [Khronos vkQueuePresentKHR requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).
- [Khronos VK_EXT_present_timing proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_present_timing.html).
- [NVIDIA Reflex 2 Frame Warp research announcement](https://www.nvidia.com/en-us/geforce/news/reflex-2-even-lower-latency-gameplay-with-frame-warp/): shows input-aware late warp is distinct from predicting a completely new scene, **not** a GSF hardware compatibility claim.
- [Qwen source review on disocclusion](GSF_QWEN_DISOCCLUSION_SOURCE_REVIEW_20261018.md).

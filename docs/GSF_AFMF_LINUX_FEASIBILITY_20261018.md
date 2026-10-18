# GSF × afmf-linux: architecture feasibility and Steam Deck gates (2026-10-18)

**Status: RESEARCH ONLY. Not a GSF implementation, release or device acceptance.**  
**Base:** GSF direct-real-WSI experiment PR #155, `550eb59ca87dfd860121a02727a23d9bac9d5812`.  
**Reference:** [afmf-linux](https://github.com/serialexperimentslainnnn/afmf-linux), MIT, [v1.3.1](https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v1.3.1), pinned main commit `bc56e5bd0e3a6c328aedef664d0d36363fb58b36`. Upstream's own hardware tests cover RDNA3/RDNA4, **NOT Deck RDNA2**.

## Decision

Investigate AFMF's **real game swapchain / extra-image / asynchronous presentation ownership** as an alternative to creating a virtual app-visible swapchain. This is the closest architectural reference for GSF direct-real-WSI #155. Preserve GSF's independent native renderer, GPU LMC and own policy stack: AFMF is a research reference, **not** an external dependency and not evidence of working 2× on Deck.

Keep the experimental source-proxy/output-owner PR #154 isolated. Do not mix its virtual image ABI with the direct-real-WSI path until each proves its own lifecycle and compositor contract. Stable main, Governor, LMC, existing Frame Fusion and external LSFG-VK backend are out of scope. Only one presentation/frame-generation owner may run in a game.

## Concrete source-level findings (upstream afmf-linux)

| Concern | AFMF source | Relevance to GSF |
| --- | --- | --- |
| Intercept a real WSI swapchain | `src/swapchain_create.c`: patch `VkSwapchainCreateInfoKHR`, allocate extra real images and TRANSFER_SRC/DST usage, optionally change app FIFO to MAILBOX *only if offered and pNext is compatible* | Avoid virtual `VkSwapchainKHR`, proxy `vkGetSwapchainImagesKHR` and game renderpass/layout remapping; test direct path #155 |
| Controlled companion acquire | `src/swapchain_cadence.c`: speculative zero-timeout `vkAcquireNextImageKHR` of a spare image, bounded retry if requested, skip companion on miss | No waiting indefinitely for available images; preserve real frame priority |
| Present hook and sync ownership | `src/swapchain.c`: app waits consumed once in an adapter `vkQueueSubmit`, then work queued to layer worker | Never pass a consumed app semaphore downstream twice; propagate failures deterministically |
| GPU producer | `src/swapchain_worker.c` + `src/swapchain_record.c`: work slot fence, own queue, history copy, flow+interpolation, signal distinct synthetic and real semaphores | Borrowed device/dispatch and queue serialisation must remain valid under Proton and Gamescope |
| Sole present scheduler | `src/swapchain_presenter.c`: present synthetic first, hold real frame for a bounded part of source interval, present real on the **same** real swapchain | Separate Vulkan WSI acceptance from proven compositor output and physical scanout; do not claim zero latency |
| Temporal generation | `src/framegen_record_frame.c`, `src/framegen_format.c`, `shaders/afmf_interpolate.comp`, AMD FidelityFX optical-flow shaders | Native color-buffer-only 2× reference: 8×8-block motion estimation, luma pyramid, scene cut, warp/blend, HUD handling; compare with GFG GPU LMC before selecting algorithms |
| Degradation policy | `src/swapchain_cadence.c`: minimum source FPS + quality/streak-controlled companion thinning | Use actual GPU readiness, refresh budget, real-frame protection; avoid optimising an unreliable present path |
| Complex extension chains | `src/swapchain_chain.c`: preserves supported `pNext` nodes and uses surface mode-compatibility query before rewriting the mode list | Essential for Gamescope maintenance1 and FROG WSI. Never force FIFO into Gamescope's driver-visible MAILBOX-only list |
| HDR pixel format | `src/framegen_format.c`: explicit BGRA/RGBA8, A2B10G10R10, RGBA16F paths | Format handling is **not** proof of accurate PQ, HDR metadata, Gamescope 10-bit path or scanout |
| Device compatibility | `src/layer_device_queues.c`: independent compute queue when available, shared app queue with locking otherwise | RDNA2 Vulkan feature and queue-family availability must be checked on Deck; 32-bit processes unsupported upstream |

**Important upstream caveats:** AFMF's `AFMF_GAMESCOPE=1` defaults to five *additional* WSI images rather than two. Its own performance notes show that some games abort when swapchain images exceed eight. GSF must NOT blindly adopt five extra images: check actual `maxImageCount`, app-observed counts/constraints and support an automatic conservative real-only fallback. AFMF itself has measured Gamescope 3.16 on RX 9070 XT/RDNA3 setups; this cannot be extrapolated to SteamOS Deck.

AFMF default interpolation generates **one** intermediate (2×). GSF's own 3× requires **two** intermediate presents per real-frame interval, additional images and coherent scheduling. No 3× claim may be inherited from AFMF benchmarks.

## Performance lessons worth experimentally reusing

1. **Zero-copy-ish ingest:** an optional single read of the game image into both color history and downsampled luma (`AFMF_DIRECT_INGEST`); validate image usage and SRGB before enabling. Never assume `STORAGE`/SAMPLED usage is supported.
2. **Bound compute:** 5 vs 7 motion pyramid levels, half-resolution flow on tight GPUs, packed `shaderInt16` SAD and static-block/prediction early exit. Verify shader feature support, motion quality and disocclusion instead of treating a feature flag as free speed.
3. **Direct GPU output** is opt-in upstream because requiring STORAGE on app swapchain can slow rendering more than an extra transfer saves. Test A/B on real hardware.
4. **Bounded pacing:** companion first, real afterwards. Measure source FPS, generated FPS, real present loss, display cadence, 1%-low, p95/p99 GPU cost, latency and TDP. A half-frame hold adds latency; the fact that app's `vkQueuePresentKHR` returns quickly does not eliminate it.
5. **Never silently count an accepted Vulkan present as OLED scanout.** Driver return codes, on-screen compositor output, and physical refresh are three separate facts.

Upstream's quoted 0.56–0.71 ms per generated 3440×1440 frame refers to RX 9070 XT (RDNA4) and its test conditions; ~1.25 ms game-time on RX 7800 XT (RDNA3). **Neither predicts performance on Deck's RDNA2 APU.** Measure Deck rather than scale these times by pixel count.

## Current GSF blocker discovered during review

At #155 head `550eb59`, the [Native FG3 GPU validation software run](https://github.com/wavessevaw/GFG-Extreme/actions/runs/39051586752) **FAILED**, although separate GSF Direct Real WSI Deck Kit, GSF Output Owner Contract, Temporal Engine and CI workflows succeeded.

Native GPU smoke log (lavapipe/X11, **not Deck**) reported `real=8 synthetic=8 skipped=6 verify_checks=0`; explicit failing assertions were `st.skipped_synthetic == 0` and `st.verify_checks > 0`. A software test with accepted synthetic WSI output, skips and no verification does not establish correct output. Classify whether skips were permitted deadlines vs unintended regression; preserve test semantics and measurement rather than weakening assertions for a green badge. This failure blocks any build promoted as a GSF release.

## Proposed work packages (in order, with hard go/no-go gates)

### G0: Isolated AFMF reference and provenance
- Pin upstream version and exact commit; compile **outside** GFG stable release. Capture compiler, Vulkan loader, Mesa, validation and SPIR-V artefact hashes.
- Run upstream headless, `vkcube`, sanitizer/validation tests with the layer scoped to the **child process**; never install or modify global SteamOS Vulkan manifests in an unattended workflow.
- Verify legal notices if reusing source: upstream MIT notice; AMD FidelityFX SDK vendored MIT notice in `shaders/fidelityfx/LICENSE.txt` and provenance in `NOTICE.md`. GFG is GPL-3.0-or-later. Preserve each copyright and license attribution. **No third-party code copied in this research PR.**

### G1: Fix native baseline and direct real WSI correctness
- Diagnose #155 failed Native FG3 smoke with timing and verification evidence; restore explicit correctness checks.
- Borrow only AFMF *ideas* initially: real swapchain, spare-acquire, adapter semaphore, separate work and present owner; retain GFG's correctly dispatched physical-device handles.
- Test original application's frame count and render/present ordering, no synthetic mode, forced GPU miss, bounded acquire timeout, Vulkan validation, device loss, resize/recreation, `oldSwapchain`, X11/lavapipe pixels, `pNext`, presentation ID/fence.
- Preserve app and downstream Gamescope mode lists. If incompatibility occurs, select original real-only WSI *before* changing ownership.

### G2: Gamescope SDR8 qualification, **2× only**
- Exercise an isolated Vulkan test app using the real loader and real RADV device; then separately a Proton game. Test supported swapchain image budgets with +2, +3, +5 **only where compatible**.
- Log: real game frame ID, generated frame ID, app wait consumed, GPU complete timestamp, spare result, driver present result, surface image ownership and compositor/scanout verdict.
- Compare clean/no layer; GSF disabled; reference AFMF disabled; GSF x2 experimental. Do not load GSF, AFMF and LSFG-VK together.
- Test Desktop, nested Gamescope (not physical scanout proof), then SteamOS Gaming Mode. Record camera high-frame-rate evidence for the OLED panel if claiming 45 real → 90 displayed.

### G3: Steam Deck OLED HDR10/packed10 + stability
- Specifically exercise `VK_FORMAT_A2B10G10R10_UNORM_PACK32`, `VK_FORMAT_R16G16B16A16_SFLOAT`, colorspace/transfer/HDR metadata and the `VkSwapchainPresentModesCreateInfoEXT/KHR` maintenance1 chain.
- Never use a device-layer physical handle with a global Vulkan loader trampoline; this previously caused GSF startup failure. Do not defeat the existing packed10 fail-open gate merely to obtain synthetic counters.
- Check no crash at game launch, resume, alt-tab, resize, low GPU memory, 30-minute sustained test, sleep/resume and profile disable. Roll back to real-only without lingering layer state.

### G4: Integrate native GSF temporal compute
- Only after **proven 2× display ownership** attach the native GPU LMC interpolation output, compare its motion/hud/occlusion results against independently built AFMF reference and evaluate selective FidelityFX flow techniques.
- Test incremental 3× with two independently signaled intermediary outputs, source retention, 90 Hz target and capped GPU budget.
- Keep Governor power feedback optional and based on measured GPU cost/frametime and real FPS, not the Steam HUD's combined FPS alone.

## Acceptance and non-goals

- **PASS** requires actual game startup and frame retention, no validation faults, pixel-verified GPU composites, measured real vs synthetic WSI sequence, camera-confirmed Steam Deck display cadence, stable 1%-low/frametime/latency, and deterministic recovery on GPU misses.
- **NOT PASS:** green CI alone, Vulkan `VK_SUCCESS`, accepted synthetic WSI submits, synthetic software X11 pixels, launcher exit 0, frame counters or third-party RDNA3/4 screenshots.
- **NO** main merge, official build, Decky backend toggle, blanket system layer installation, LSFG-VK replacement, or claim of 2×/3× on Deck from this document.

## Primary sources

- https://github.com/serialexperimentslainnnn/afmf-linux/tree/bc56e5bd0e3a6c328aedef664d0d36363fb58b36
- https://afmf-linux.digitalexperiments.dev/configuration/
- https://afmf-linux.digitalexperiments.dev/performance/
- https://afmf-linux.digitalexperiments.dev/faq/
- https://github.com/wavessevaw/GFG-Extreme/pull/155
- https://github.com/wavessevaw/GFG-Extreme/pull/154
- https://github.com/wavessevaw/GFG-Extreme/actions/runs/39051586752

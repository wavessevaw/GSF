# GSF × AFMF-Linux: source-level deep audit, Vulkan WSI and RDNA2 adoption plan

**Date:** 2026-10-18. **Status:** architecture and code reading only; no Deck hardware or native runtime qualification in this report.
**Pinned external reference:** `serialexperimentslainnnn/afmf-linux` MIT v1.3.1, main `bc56e5bd0e3a6c328aedef664d0d36363fb58b36`.
**GSF comparison head:** `550eb59ca87dfd860121a02727a23d9bac9d5812` in `experiment/gsf-real-wsi-direct-owner-20261017` (#155), child of #154 source-proxy experiment. Do not treat this report as an instruction to merge or ship.

## Executive finding

AFMF is a **real WSI companion-insertion architecture**, not a virtual app swapchain implementation. Its biggest value for GSF is the proven-on-other-hardware *ownership pattern*: spare real swapchain image, one-time consumption of game wait semaphores, work-queue submission, independent presentation thread and bounded fallback. It is not a validated drop-in Steam Deck architecture. Importantly, AFMF deliberately converts FIFO to MAILBOX on compatible surfaces while GSF direct-real-WSI treats ordered FIFO as a prerequisite and is careful not to push FIFO into Gamescope's driver-facing MAILBOX contract.

**Decision:** prototype this as an independent GSF 2× research architecture behind opt-in flags, with unchanged main branch and completely independent HDR+Gamescope gates. Do not merge AFMF's interpolation algorithm and WSI simultaneously: isolate the source-of-truth for any eventual performance gain.

## 1. Exact upstream data path

[Create swapchain](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/swapchain_create.c):
- `generation_blocker` requires a supported single-layer real surface and TRANSFER_SRC + TRANSFER_DST capability. It rejects when `minImageCount + extra_images > maxImageCount` (for a nonzero max).
- Requests +2 images normally and +5 when `AFMF_GAMESCOPE=1`; queries optional SAMPLED/STORAGE usage only when device+surface+format are compatible.
- For app FIFO/FIFO_RELAXED, it can request MAILBOX instead, **only when the surface offers it and the VkSwapchainPresentModesCreateInfoEXT pNext chain is compatible**; it retains original otherwise.
- If Gamescope itself inherits `AFMF_GAMESCOPE=1`, process detection makes the layer passive inside Gamescope. The game can still get it in its own process.
- The Vulkan swapchain remains real; `vkGetSwapchainImagesKHR` receives actual downstream images, avoiding the virtual-layout and renderpass provenance work required by GSF #154.

[Present interception](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/swapchain.c):
- Only a recognized, **single-swapchain** present can use this path. Multi-swapchain and unsupported state go to pass-through. Dynamic present pNext shapes are carried selectively.
- The game wait semaphores are consumed exactly once by an **immediately submitted adapter VkQueueSubmit**; an internal per-slot semaphore is handed to the worker. Then the app hook can return while actual output is delayed.
- In-flight jobs are bounded to swapchain image count; under backpressure `pthread_cond_wait` can **block the game hook**. Never equate a short average host present with hard guaranteed nonblocking operation.
- On worker failure after consuming waits, the code tries to forward `sem_app` to `sem_real`, and if that GPU submit fails, can present the real frame with `real_unwaited=true` and no wait. This is a **high-priority failure-injection/visibility risk to test**, not a claim of an observed bug.

[Source and companion GPU work](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/swapchain_worker.c):
- Slot fence prevents command-buffer reuse while the old submit is running.
- The work thread opportunistically takes a preacquired spare real swapchain image. No free image => no companion. Missing prior history and governor decisions also suppress companions.
- GPU command buffer transitions actual game image from PRESENT_SRC to read layout, performs source read and flow, writes companion, restores both PRESENT_SRC layouts, and signals separate real+companion binary semaphores.
- First real frame only fills history; there is no companion before two real frames are available.

[Acquire image and fair locking](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/swapchain_cadence.c):
- Presenter speculatively calls `vkAcquireNextImageKHR` with zero timeout to reserve the next companion slot. Worker can wait in <=1 ms slices without monopolizing the WSI mutex, using `wsi_turn` and `wsi_lock`.
- Acquire uses `spare_fence`. The worker must not write before that acquire is complete; the companion returns to downstream WSI only when PRESENT_SRC and signaled.
- A timestamp/counter showing acquisition **is not** a timestamp of physical display.

[Actual presenter](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/swapchain_presenter.c):
- Calls real `vkQueuePresentKHR` for generated image **before** real image; waits on their respective per-image binary semaphores.
- Keeps app present IDs, optional present fences and timing metadata on **real** only; synthetic carries no app ID; unknown pNext forces inline or passthrough where possible.
- A bounded half-frame hold after GPU work aims at even pacing. This **adds latency** to the real frame by design and is partly compensated by skipping stale companions.
- Queue present requests are ordered, but compositor/scanout scheduling is not guaranteed by Vulkan return codes.

[Device and queue management](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/layer_device.c):
- Intercepts vkCreateDevice to add an async queue, optionally shaderInt16, with fallback to the game's original device create if alterations fail.
- Requests high global queue priority only when extension available, retries without it on failure, or serializes a shared app queue; no assumption Deck grants CAP_SYS_NICE.
- Correct originating-instance `next_gipa` + loader callback stamping matter; GSF previously had borrowed physical device dispatcher failures (#151).

## 2. Optical-flow and pixels (algorithms cannot be evaluated by WSI counts)

Source: [framegen](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/framegen.c), [frame recording](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/framegen_record_frame.c), [interpolator](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/shaders/afmf_interpolate.comp), [formats](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/src/framegen_format.c).

- Uses AMD FidelityFX Optical Flow SDK shaders plus a custom GLSL warp/blend shader. Input is the **color buffer only** (no game depth or true motion vectors). Coarse-to-fine optical flow at 8×8 blocks; five/seven pyramid levels, optional half-resolution flow (16-pixel blocks at output), SAD/int16 and early static-block/prediction decisions.
- Synth shader samples previous at `y+v/2` and current at `y-v/2`, averages them, and leans toward unmoved current value where two warps disagree. HUD equal-color detector keeps static overlays. On scene cut or >configured motion ceiling it repeats/blends rather than doing trusted motion interpolation.
- **Misleading-counter trap:** if `afmf_framegen_create` returns NULL, `afmf_sc_gen_init` falls back to `create_history` and `afmf_sc_record_frame` copies the previous real frame into the companion. The worker can still increment `generated` on companion present. This path yields *duplicate* content, not optical-flow interpolation. GSF must separately count `synthetic_qualified`, `duplicates`, `scene_cut_blends` and `wsi_accepted`.
- `VK_FORMAT_A2B10G10R10_UNORM_PACK32` is accepted in upstream; its format path says 8/10-bit encoded values go into Rec.709 luma. The interpolator averages samples directly. In PQ HDR10, arithmetic on encoded values is generally not equal to linear-light synthesis; a valid swapchain format **does not demonstrate HDR photometric correctness**. Scope a numeric golden-image test: PQ EOTF, linear blending, OETF, highlight roll-off, near-black and alpha, plus metadata forwarding. scRGB float has distinct luminance path and merits separate tests.
- The motion-flow limit and 8×8 block behavior will have conspicuous errors at rapid occlusion and HUD-on-moving-background edges; evaluate frame image *quality*, not accepted WSI throughput alone.
- `AFMF_DIRECT_INGEST=1` can combine a GPU read of real image and luma ingest; requires SAMPLED usage. `AFMF_DIRECT_OUTPUT=1` avoids a copy but requires swapchain STORAGE usage; upstream disables by default, as it can interfere with game performance. Neither is a universally safe optimisation.

## 3. Fundamental WSI trap: FIFO and MAILBOX are not interchangeable

- AFMF tries FIFO→MAILBOX when possible. MAILBOX may replace/drop queued frames, so perfect API present counts can coexist with missing displayed synthetic frames.
- GSF #155 `native_ineligible_reason` requires FIFO; an optional forced-FIFO conversion is refused when downstream Gamescope pNext list forbids it. This refusal is correct as a driver contract and **must not be bypassed** to produce a success-looking counter.
- Gamescope often presents app FIFO scheduling above driver-facing MAILBOX and carries `VkSwapchainPresentModesCreateInfoEXT` maintenance1 chain; the exact GFG Deck logs already found `pnext_type=1000275002` and failures in the virtual output owner. Test ordering both `GFG`/Gamescope Vulkan layers, no global implicit install, no forced driver modes.
- AFMF's successful nested Gamescope RDNA3/RDNA4 runs do not establish Deck embedded compositor scanout or pixel order. One real downstream swapchain is necessary but not sufficient for physical cadence proof.
- Formal spec: [vkQueuePresentKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html) describes enqueued presents and presentation engine scheduling; [Vulkan Guide semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html) warns that present-wait binary semaphores cannot safely be recycled from a mere GPU submit fence. Reacquire of the same swapchain image plus completed acquire sync, or explicit supported present completion, is key.

### Explicit WSI invariants to instrument in GSF

1. Every app binary wait semaphore has **one** consuming GPU wait submission and is never replayed after consumption. Count `app_wait_submit_accepted` vs `app_wait_replayed`; latter must be zero.
2. No generated image is presented until acquire sync completes, layout is PRESENT_SRC, and its own ready semaphore has a submitted signal.
3. No real or synthetic ready semaphore is re-signaled until its previous present wait is proved retired by reacquire/present-fence semantics.
4. All downstream WSI presents belong to exactly one explicit output owner per swapchain; their handle is the **real** VkSwapchainKHR.
5. Game present IDs, present fences, timing structures and dynamic present modes are neither silently dropped nor reused on generated frames.
6. On incomplete pNext / unsupported format / image count cap, fall back to *original real-only output before taking ownership*. After consuming game waits, only correctly synchronized recovery is permitted.
7. Preserve original game 1%-low, input/real-frame latency and successful game launch separately from output FPS.
8. Original driver-facing Gamescope mode list must stay valid. Opt-in FIFO rewrites are forbidden if the lower chain excludes FIFO.

## 4. Current GSF baseline and CI diagnosis

**#154 native baseline already fails before #155**:
- [#154 run 39040854737](https://github.com/wavessevaw/GFG-Extreme/actions/runs/39040854737): Native FG3 GPU validation step `Bounded async output, lifecycle, metadata and actual intermediate pixels` FAILED.
- [#155 run 39051586752](https://github.com/wavessevaw/GFG-Extreme/actions/runs/39051586752): same step FAILED; other Output Owner/Direct WSI workflows green.
- In both 8-real-frame, `GFG_NATIVE_ALGO=lmc-gpu`, `GFG_NATIVE_GPU_BRIDGE_VERIFY=1`, async software lavapipe cases: `real=8 synthetic=8 skipped=6 holds=4 verify_checks=0` and assertions `skipped_synthetic == 0` and `verify_checks > 0` FAIL.
- The failing scenario is explicitly configured in the workflow with verification every 2 frames and no allow-skips flag. The bridge's output counters alone don't diagnose whether underlying late GPU work, no readback at retirement, synchronization or test expectation is responsible. **Do not remove assertions to turn the workflow green.** Reproduce with per-slot deadlines and per-frame verify instrumentation; classify expected software delay vs real regression. Test if the readback step has genuine verification windows under async output.
- The observed failure predates #155 and **cannot automatically be attributed** to #155.

## 5. Deck physical constraints and sampling math

At Steam Deck OLED 1280×800:
- An extra 4-byte RGBA8/packed10 swapchain image is 4,096,000 bytes (~3.91 MiB), excluding driver alignment, history, compute intermediates and memory overhead. +2 ≈7.81 MiB; +3 ≈11.72 MiB; +5 ≈19.53 MiB of raw image storage.
- RGBA16F is 8 bytes/pixel: +5 raw images ≈39.06 MiB. This is a **lower-bound illustrative cost**, not a measured VRAM budget.
- A target of **45 real → 90 physical** uses 22.22 ms between real frames and 11.11 ms per output slot. Late generated work must be dropped; simply delaying real frames to preserve a target FPS makes latency worse. For **30 real → 90 physical** at 3×, two synthetic slots per real pair likewise have 11.11 ms spacing and a larger GPU workload.
- RDNA4 performance claims in AFMF docs (0.45–0.7 ms at ultrawide resolution depending on instrumented stage) and RDNA3 measurements cannot be extrapolated to Deck's 8-CU RDNA2 APU. Run actual GPU timestamps for `ingest / pyramid / flow / interpolator / copy / present`, plus memory bandwidth/clock/power.

## 6. Go/no-go stages for independent GSF

**A. Reference reproducibility**: pin upstream MIT code and AMD FidelityFX notices; compile and run standalone with 64-bit Vulkan loader and validation. Record shader formats/features, disabled and 2×, correct output pixels. No global Vulkan manifest injection; no third-party redistribution inside GFG package at this stage.

**B. GSF direct WSI deterministic state tests**: keep GFG layer, LMC and output logic independent; create a per-image lifecycle trace:
`FREE → APP_ACQUIRED → RENDER_SIGNAL_QUEUED → SOURCE_COPY → SYNTH_READY → PRESENT_SUBMITTED → REACQUIRED`. Include forced acquire-unavailable, submit failure after waits were consumed, no available image, multi-swapchain presents, pNext unsupported, queue-sharing, shader-failure and cleanup. Stable, no thread hangs.

**C. Steam Deck SDR8 2×**: force no other FG owner; compare clean, GSF off, standalone AFMF reference and experimental native GSF using *the same* controlled scene. Log app source FPS and real frame IDs, synthetic validated pixels and IDs, present submission sequence, rates, queue wait age, per-stage GPU timestamps and actual camera-observed panel frames. Test in Desktop, nested Gamescope, Gaming Mode separately.

**D. Steam Deck OLED HDR10/packed10**: after SDR8, test FROG WSI two layer orders, exact maintenance1 pNext and downstream MAILBOX constraints, device physical handle dispatch, PQ and scRGB colorspace, repeated resume/recreate and corrupted/unknown pNext. Any crash/black frame is immediate no-go, not permission to suppress validation.

**E. Independent GSF LMC comparison**: measure GFG GPU LMC against standalone AFMF Flow as a controlled reference, stratified by camera pan, UI/HUD, fine movement, particles, fast disocclusion, lighting cuts and static scene. Evaluate 5/7-level and half-size flow techniques on Deck using measured quality/motion costs. 3× only after 2× output contract is proven.

## 7. Candidate priorities and non-goals

| Priority | Change | Evidence gate |
| --- | --- | --- |
| P0 | Explain and fix inherited Native FG3 async LMC software smoke failure | No skipped outputs in an *appropriate deterministic* acceptance workload; positive pixel verification; separate intentional skip tests |
| P0 | Formal real swapchain semaphore and image-state trace | Khronos validation zero errors in both layer orders, forced failure cleanly falls back |
| P0 | Gamescope maintenance1 mode list / packed10 crash containment | Real-only game startup and no incompatible injected mode |
| P1 | Borrow AFMF speculative spare acquire + queue ownership ideas | 45→90 SDR pixel sequence without losing real frames on Deck |
| P1 | Profile Flow/ingest and flexible compute budget | GPU timing and power/quality A/B on Deck RDNA2 |
| P1 | HDR linear-light reconstruction and color validation | Explicit PQ/scRGB numeric golden test and separate OLED validation |
| P2 | GSF native 3× on proven output owner | Both distinct intermediates, correctly ordered, latency/power within measured budget |

**Non-goals:** do not enable multiple generation layers simultaneously; do not assume AFMF's native AMD GPU speed holds for Deck; do not copy arbitrary source into GPL GFG without MIT/FidelityFX attribution; do not claim OLED scanout from `VK_SUCCESS`; do not alter stable GFG/Governor/LSFG-VK/LMC during research.

## References

- AFMF source: https://github.com/serialexperimentslainnnn/afmf-linux/tree/bc56e5bd0e3a6c328aedef664d0d36363fb58b36
- AFMF documented test hardware: https://afmf-linux.digitalexperiments.dev/tested/
- AFMF Gamescope/limits FAQ: https://afmf-linux.digitalexperiments.dev/faq/
- Khronos WSI: https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html
- Khronos semaphore reuse: https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html
- Vulkan swapchain maintenance 1: https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_swapchain_maintenance1.adoc
- GSF #154: https://github.com/wavessevaw/GFG-Extreme/pull/154
- GSF #155: https://github.com/wavessevaw/GFG-Extreme/pull/155


## 8. Further negative research results and protection against cargo-cult optimisation

AFMF's [performance.md](https://github.com/serialexperimentslainnnn/afmf-linux/blob/bc56e5bd0e3a6c328aedef664d0d36363fb58b36/docs/performance.md) actually records several **failed optimisations**. Do not repeat them unquestioningly in GSF:
- A second compute queue provided no gain when the entire generation workload finished before the next source frame arrived.
- Forcing wave32 gave no measured gain; GPU-specific subgroup width needs benchmarking.
- Replacing the search with a narrower ±4 local refinement (instead of ±8) made the coarser vectors less accurate; finer levels searched more, worsening measured time.
- Running the scene-change divergence detector concurrently with the coarse search caused an early-return/barrier inconsistency and hung Intel GPUs. Keep correct compute synchronization even when targeting RDNA2.
- Default direct STORAGE output could slow game rendering more than it saved on the output copy, so upstream made it opt-in.
- The author's full benchmark at 3440×1440, RX 9070 XT, five pyramid levels / half-resolution flow quotes 560–610 µs GPU per frame; RX 7800 XT records 1,070 µs headless and 1,245 µs in-game. Treat each as **upstream measurements under those exact conditions**, not estimates for Deck.

## 9. Two extra GSF audit hypotheses exposed by this comparison

**H-ENV (packed10 guard activation):** `native_gamescope_packed10_unsafe` in GSF `src/layer.c` relies on a text search in **`VK_INSTANCE_LAYERS`**. A loader may activate layers implicitly via manifests or alternative loader settings, without this string. Therefore, this guard's *detection completeness* is unproven even when its specified explicit-layer case is correct. Test implicit and explicit Gamescope WSI layer activation separately. Never remove the conservative packed10 fallback while changing detection.

**H-CAP (unknown surface caps):** The GSF direct-WSI candidate path can call `gfg_real_wsi_can_extend` with a zero max when the actual `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` result was not available; zero correctly denotes Vulkan's *reported unlimited maximum*, but an **unknown result** is not the same fact. A candidate's first extended create has a real-driver failure fallback, yet the eligibility/log reason should distinguish `unbounded cap reported by driver` from `cap not measured`. Test both cases and ensure no extra-image assumption is treated as established when caps lookup failed.

Neither hypothesis is asserted as a field-observed new crash. Both are specific inspection targets with reproducible simulation gates.
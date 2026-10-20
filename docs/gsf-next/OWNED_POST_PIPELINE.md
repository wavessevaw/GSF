# GSF: post-generation output owner pipeline (2026-10-20)

**Status:** architectural implementation plan, not a running game-path implementation. Do not ship, merge, or claim ×2/×3 until hardware acceptance. This branch is stacked on experimental PR #59 only to reuse the existing optical-flow engine and GPU-stage split.

## Decision / why a deeper buffer failed

The reported Deck test found a useful intermediate queue depth, followed by progressively worse stability as depth increased. Earlier physical Deck traces at about 45–50 source FPS / 90 Hz had about 66 Vulkan present calls per second rather than 90, with many delayed generated pairs. That is a throughput/ownership problem, not evidence that more queued frames will help.

Today's path:
```
game WSI acquire/render -> app present hook -> worker (ingest + flow + warp)
    -> private synthetic images -> presenter acquires GAME WSI extra images
    -> synthetic WSI present(s) -> delayed REAL WSI present
```
Even the experimental split-ingest stage still submits reconstruction on the same GPU queue, and keeps the game WSI image acquired until the real presentation. The producer and consumer share backpressure and frame lifetime. If output service rate is less than arrivals, a larger queue stores *latency debt* instead of creating throughput. The current ordinary (non-split) path additionally waits for full reconstruction before releasing the real endpoint.

**Decision:** build one post-game output owner with **its own real downstream swapchain** and a **virtual source swapchain** for the game. Adapt the already existing `engine/gfg-native/src/output_owner.h`, `virtual_wsi_facade.h` and `GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS` research in `docs/GSF_OUTPUT_OWNER_PROTOTYPE.md`; do not duplicate the unfinished facade under another unrelated name. The production GSF Next optical-flow/warp implementation becomes a replaceable private texture producer. A post-processing Vulkan overlay is *not* an output owner.

## Contract and pipeline

```
Game: vkAcquireNextImageKHR -> render -> vkQueuePresentKHR
               |                 (virtual swapchain, offscreen VkImages)
               v
[Source adapter] consume game render semaphore exactly once
    -> GPU snapshot into pinned real A/B image ring
    -> signal snapshot done; release virtual image to game
               |
               v
[Frame processor] source pair IDs + capture timestamps
    -> optional optical flow once per A/B pair
    -> optional 1..3 warp outputs, each with ready fence
    -> late synthetic is CANCELLED before final WSI acquire
               |
               v
[Single output owner] sole vkQueuePresentKHR issuer for real downstream swapchain
    -> one bounded, refresh-aligned deadline stream
    -> choose chronological real/synthetic frame when ready
    -> skip expired synthetic rather than delaying next real
               |
               v
real compositor surface (reuse game's VkSurfaceKHR when eligible)
    -> Gamescope -> panel
```

1. Game-facing swapchain handle is virtual. `vkGetSwapchainImagesKHR` returns private renderable images, `AcquireNextImageKHR/2` signals exactly as Vulkan requires, `vkQueuePresentKHR` snapshots the finished image instead of forwarding the virtual handle to ICD. Source image availability is governed by snapshot retirement, **not** downstream presentation.
2. The downstream WSI swapchain must be the only visible output on the existing game surface. No simultaneously displayed game swapchain, floating window, HUD trick, unsupported Gamescope screenshot API, or recursive capture. Surface capabilities/present extensions must be negotiated separately.
3. Source image data and fences are retained in a bounded A/B history until GPU consumers retire; both source and synthetic have immutable pair IDs, source capture time, ready time, output target and stage timing.
4. Optical flow and warping must never block the game-source image recycle after snapshot completion. A separate queue alone is insufficient if it shares the same saturated GPU: generation admission must use measured GPU cost and the real deadline.
5. Output schedule is *owned* rather than being derived from successive late application presents: only one authority picks frame order and WSI acquire/present. Clock synchronization uses supported compositor feedback when available; plain `vkQueuePresentKHR` success is not display evidence.
6. **Bounded age rather than growing depth:** real-source capture ring is bounded; synthetic records can expire, be superseded or be skipped *before* WSI acquire. New source frames must not inherit ever-growing presentation backlog. If source production exceeds screen refresh, all real source frames cannot physically be scanned out; record which ones are coalesced/dropped by compositor rather than pretending otherwise.
7. If no second source frame or synthetic ready fence arrives, release the real available frame on its bounded real-only deadline. At scene cuts, resize, device loss, 10-bit incompatibility or frame-ID discontinuity, cancel dependent synthetic and retire all resources deterministically.

## Hard Vulkan blockers to implement before game attach

- Intercept `vkCreateSwapchainKHR` and `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`, `vkAcquireNextImage2KHR`, `vkQueuePresentKHR`; safely handle image count queries and `VK_INCOMPLETE`.
- Translate PRESENT layouts **only for registered virtual images** across `vkCmdPipelineBarrier`, Sync2 variants, renderpass / renderpass2 initial/final layouts, attachment views/framebuffers and dynamic rendering. No rewriting of actual output WSI images; refuse unsupported command/feature shapes rather than corrupting application commands.
- Preserve correct binary semaphore consumption, app acquire fences, present ID, present wait(2), present fence, `pNext`, queue/device-idle, queue-family ownership, present mode, format, HDR, recreate with `oldSwapchain`, out-of-date and device-loss behavior. The facade prototype is not a substitute for these API hooks.
- The original game WSI must be selectable as an atomic, early fail-open fallback **before** any game semaphore is consumed. Mid-frame failure with consumed semaphores requires owned safe retirement and must not resubmit a binary wait.
- Guard implementation behind an opt-in build/runtime capability gate; unsupported Proton/DXVK/VKD3D, multi-swapchain, packed10 and Gamescope layer-order paths stay real-only rather than being treated as qualified.

## Output/FG throughput, not multiplier labels, determines success

At 90 Hz, 45 source FPS ×2 ideally means **45 distinct synthetic + 45 real** per second. ×3 with the same source cannot make the 90 Hz panel show 135 frames; at 90 Hz a useful ×3 scenario is 30 source FPS + 60 unique synthetic. Likewise 22.5 ×4 reaches 90. Generated GPU images, WSI-accepted presents, compositor predictions, compositor discards, KMS-flip observations and optically distinct scanout are **separate metrics**.

- Budget each A/B pair as ingest/copy + one OF analysis + (multiplier−1) warp calls + output copy/present; measure each component. The generator on a 99% busy GPU may simply be too expensive. Do not promise ×2 by scheduling when measured compute throughput is insufficient; downgrade multiplier, flow resolution or reject synthetic safely.
- Never count duplicates as distinct synthetic pixels. Record source FPS compared with real-only baseline, accepted real/synthetic source IDs, WSI gaps (p50/p95/p99), age at present, pair latency, source backpressure, GPU busy and compositor dropped output.
- Hard test matrix: 45→90 ×2, 30→90 ×3, 22.5→90 ×4; 60→90 capped output, scene changes, GPU saturated, 10-minute stable gameplay, pause/resume, resolution change, HDR/SDR feature gating, several Proton versions, and restore real-only on disable.
- **Success gate:** no upward trend in queue age/latency or blocked app presents through the run; no missing app present/compositor contract; distinct synthetic pixels actually alternate at target interval on a physical Deck capture; meaningful sustained synthetic rate and stable source FPS, not only WSI acceptance. Numeric p95/p99 thresholds shall be fixed from the baseline before scoring; do not relax failures by dilating CI clocks.
- Prototype tests on Lavapipe/Xvfb prove API/pixel/lifetime semantics, **not** physical 90 Hz. The user is not required to run repeated unqualified manual builds.

## Implementation stages / ownership

**P0 output transport:** port/integrate the existing GFG native virtual WSI facade + real output-owner contract into a guarded GSF Next adapter. First demonstrate *game-source offscreen → real private WSI* for a controlled Vulkan app with generation disabled. Game app must not use downstream WSI images; only output owner does.

**P1 independent lifetime:** GPU snapshot A/B, source acquire release, asynchronous fences and bounded queue. Verify producer can continue while output pauses, and that consumer recovers without unbounded age.

**P2 graft generator:** connect existing GSF Next ingest/optical flow/warp to private source and output textures, with explicit readiness per pair; source snapshot signals game independence, slow synthetic never blocks real.

**P3 pacing + governor:** fixed output deadline stream, age-based cancellation, adaptive admission based on throughput rather than a proportional queue. Apply a small, hysteretic max-hold based on actual measured load. Do **not** restore Auto 8 by refresh scaling.

**P4 Gamescope/Deck acceptance:** qualified loader order, compositor surface semantics and timing feedback, pixel-sequence observation, restore and package controls, extended soak, actual 2×/3× only after measurement. Maintain real-only fallback and a separate draft PR until passing.

## Related existing source

- `docs/GSF_OUTPUT_OWNER_PROTOTYPE.md` — the actual prior prototype and exact remaining blockers.
- `docs/archive/foe-20261017/docs/gsf-output-engine/ARCHITECTURE.md` — previous architectural debate and caveats.
- `docs/research/GSF_GAMESCOPE_PRESENT_TIMING_20261018.md` — compositor prediction vs actual physical display.
- `engine/gfg-native/src/output_owner.h` and test-only `src/virtual_wsi_facade.h` — proof-of-contract code to reuse.
- `engine/gsf-next/src/swapchain_presenter.c`, `swapchain_worker.c`, `output_ring.c` — old shared-WSI pipeline; not a real independent output owner.

## Implementation checkpoint (P1 partial, 2026-10-20)

Experimental isolated-loader code in engine/gfg-native now supports GPU LMC
×2/×3/×4 on a virtual game source with one downstream X11 Vulkan output
swapchain. ×4 was verified with 8 real and 21 pixel-distinct GPU intermediate
images. An optional capture-age budget reduces synthetic output rather than
adding backlog, while retaining the real endpoint.

The new src/owned_snapshot_ring.h and tests/owned_snapshot_ring.c implement
the ownership state machine for the next step. Once an actual snapshot GPU
fence signals, a virtual game image may be released while the separate GPU
snapshot stays pinned for pending real/synthetic output. The ring is bounded
at eight; it never overwrites pinned images or assumes a missed GPU deadline
has completed. Overflow is an explicit admission failure.

**Hardware integration remains pending:** the current test-only capture worker
still completes output before retiring the game's virtual source image. This
code proves the resource-lifetime **contract**, not actual independent
source retirement. A distinct output worker with correct semaphore and
image lifetime retirement must now be connected. Also, a paced 22.5→90
software X11 lab result is not a Steam Deck OLED scanout measurement.

## Initial pair priming experiment (2026-10-20)

The first 22.5→90 x4 software X11 run **FAILED output spacing**, despite 8/21
unique real/intermediate pixels. A 44ms hole recurred between real B and
the next pair's first synthesized BC. This is causal: interpolation requires
the next real C, while the old owner had displayed B before C arrived.
The test's previous 35ms max-gap gate was correctly kept as a failure.

The new test-only `GFG_NATIVE_TEST_PIPELINE_HOLD_FIRST=1` holds initial
A until B is captured, then emits A, AB1/AB2/AB3, B at output-clock ticks.
This spends a **fixed one-source-period startup delay**, not a continually
growing frame queue. The X11 observer explicitly detects the initial real A
once B arrives instead of falsely claiming it was visible at first capture.
The same 35ms max-gap gate remains; no validation limit was relaxed.
This is an isolated lab pacing experiment; it is not yet a production output
scheduler or OLED measurement.

## Second strict x4 cadence failure and stage attribution

The prime-first-A experiment did not satisfy the unchanged 35ms X11
output gap gate: observed max 45.291ms and mean 18.390ms at simulated
22.5Hz source / 90Hz output. All 21 GPU LMC intermediates were distinct,
so rendering correctness alone does **not** establish timely throughput.
The old worker still releases the game source only AFTER all output calls;
the next source capture and pair generation are consequently serialized
behind the preceding output. This is a critical blocker, not an issue
to silence by increasing FIFO depth or inflating timing allowances.

Added opt-in per-pair stage telemetry under
`GFG_NATIVE_TEST_POST_STAGE_TIMING=1`: GPU verify/readback wall-time,
output-copy/pace/WSI wall-time, and total time since snapshot worker
callback. These measurements are software-CI timings, not Deck metrics.
The strict output gap threshold remains unchanged and may fail until
source image release and the independent output-owner queue actually
operate concurrently.

## Final generator ownership: Alpha only (user correction, 2026-10-20)

Final synthesis must be the GSF Next Alpha generator: FidelityFX Optical
Flow + the shaders in engine/gsf-next/shaders/afmf_interpolate.comp.
Output Owner supplies only the virtual game swapchain, private GPU snapshot
storage, bounded lifetime/age controls and the SINGLE downstream WSI owner.

**GPU recording bridge, implemented now:** engine/gsf-next/src/owner_alpha_record.h
and owner_alpha_record.c record two distinct GPU command buffers against
owner-supplied PRIVATE VkImages. The first copies/samples the immutable
Output Owner snapshot into Alpha colour/luma history; the second invokes
the existing Alpha optical-flow reconstruction for 0..3 output phases
and leaves private targets in TRANSFER_SRC_OPTIMAL for owner WSI copy.
Alpha neither acquires nor presents downstream swapchain images.
The snapshot is never transitioned to PRESENT_SRC or written by Alpha.
The Output Owner must submit on the same compatible queue, synchronize
source reuse after the ingest fence, and retire generated resources
after presentation.

For zero admitted synthetics the second command still updates Alpha
history and scene-change state. Invalid layouts, phase order, descriptor
indices and capacities are rejected before any GPU recording.

The previous native-LMC ×4 software lab remains historical evidence
about transport only and is NOT part of this final architecture. Do not
link LMC / Flow Lite 2 into the new generator/output owner runtime.

Current bridge is compiled into Alpha and has deterministic command-level
unit coverage. **It is not yet wired to the native Output Owner Vulkan
queue or real Gamescope surface**. Integration requires one Vulkan-layer
dispatcher and owner-private resources registered as source and output
images with real GPU lifetime fences. No new Deck candidate or claimed
×2/×3/×4 performance until completed and measured on hardware.


## Real Vulkan owner separation checkpoint (2026-10-20)

The native **Output Owner transport**, with Alpha generation disabled,
now has an opt-in, loader-installed Vulkan path
`GFG_NATIVE_TEST_DETACHED_OWNER=1`:

- Game renders into the virtual app swapchain, `vkQueuePresentKHR`
  submits a GPU snapshot copy, consuming application render waits once.
- A capture worker observes the COPY fence, transfers the private GPU
  snapshot token into an eight-image independently pinned ring, and
  retires the virtual app image. No downstream WSI acquire/present is
  performed in the capture callback.
- A second worker acquires/presents on the *sole* real downstream
  swapchain, later retires the output GPU fence, and frees the snapshot.
- A real Vulkan/X11 test deliberately adds 40 ms **after snapshot
  handoff**, verifying that source GPU-copy retirement occurs earlier
  than WSI output, that all eight originals arrive, and that no
  snapshot is left pinned. It does not claim physical OLED timing.

The detached test rejects LMC and test A/B composite. Its purpose is
to establish the transport, *not* synthetic generation.

The Alpha producer at
`engine/gsf-next/src/owner_alpha_record.{h,c}` separately compiles
and passes command-recorder tests; it records the actual FidelityFX
Optical Flow into owner-private targets. The work still remaining is a
**single Vulkan layer runtime** that registers the detached private
snapshots as Alpha inputs, puts the Alpha ingest/reconstruction GPU
commands on the same correctly synchronized queue, and hands the
resulting target images to this WSI worker. Do not start two unrelated
layers as a substitute for this integration.

The previous mandatory native output-owner CI includes a deliberately
**failing** strict 22.5→90 x4 LMC pacing experiment. It is historical
research and does not measure Alpha. Preserve its red status explicitly
rather than claiming full PR green.


## Alpha native Output Owner lifetime API, P1 staging (2026-10-20)

`engine/gsf-next/src/owner_alpha_transport.{h,c}` provides a bounded,
non-WSI, testable interface for the *actual* Alpha FidelityFX producer.
The native Output Owner's offscreen GPU snapshot is one physical image,
but it has independent logical owners:

- **Game app VkImage**: native copy worker releases to the game's virtual
  Acquire as soon as the snapshot-copy GPU fence signals. This does not
  mean the output image has been shown.
- **Output Owner REAL pin**: holds the private snapshot until the original
  frame's downstream output-copy GPU fence completes. It must also
  retain downstream WSI image ownership until actual compositor release.
- **Alpha ingest pin**: holds a **separate reference to that same private
  snapshot** until the dedicated Alpha ingest fence signals. The Alpha
  callback `release_source` MUST release only Alpha's pin, **not** the
  snapshot allocation while the owner's real pin remains. Never release
  the app's render semaphore twice.
- **Alpha generated target pins**: the independent reconstruction fence
  proves OF/warps complete; ready synthetic targets are offered to
  Output Owner only when before an immutable absolute deadline.
  `gsf_alpha_owner_output_done(...,output_copy_done)` now checks the
  actual owner output-copy VkFence nonblocking (timeout zero) and rejects
  premature reuse. WSI present return alone is insufficient.
- **Late or absent synthetic**: optional output is dropped rather than
  acquiring WSI after its deadline. Original delivery is a separate
  mandatory owner obligation and cannot depend on a successful warp.

The `owner_alpha_transport` tests model 12,000 frames, delayed ingest
vs reconstruction, deadlines, output-copy fence, bounded work slots,
stale completions and partial GPU submit failure quarantine. No actual
scanout is claimed by these mocked fence tests.

### Still missing from production

The native detached real-WSI worker and Alpha recorder/lifecycle are
not yet instantiated **inside the same Vulkan layer/device dispatch**.
The integration must register the owner's snapshot `VkImage` views with
`afmf_framegen_create`, allocate and track the Alpha output ring and
join the two separate fence streams to the owner's presenter. It must
honor Vulkan loader data, dynamic rendering layouts, device features,
queue external sync, present extension chains, and safe restoration.
The separate CI suites prove contracts, NOT a working combined Game Mode
frame generation installation. The draft PR must not be shipped.


## External architecture reference and independent GPU cost guard (2026-10-20)

Examined separately supplied **LSFG-VK** source files (VSYNC Vulkan layer,
pipeline signature v3_1, benchmark, Vulkan helpers and diagnostics) as a
**read-only comparison**. These files visibly carry
`SPDX-License-Identifier: CC-BY-NC-ND-4.0`. No LSFG-VK code, shader
source, bytecode or pipeline signature is copied into GSF.

Architectural observations, not claims of transferable implementation:

- LSFG-VK's vsync path explicitly inserts multiple downstream
  acquire/copy/present operations in its game present path, assuming a
  stable rate and exact divisor of refresh. It is unsuitable for our
  independently scheduled owner and unstable Deck source FPS.
- Its explicit pipeline signature distinguishes externally pinned sources
  and results from short-lived intermediates. Our own private ring already
  supports lifetime pinning, but physical-GPU memory traffic and resource
  occupancy still need proper measurements.
- Its standalone benchmark multiplies synthetic throughput by the requested
  factor to report a theoretical output FPS. That number is NOT Gamescope
  WSI present completions, unique scanout pixels or game FPS retention.
  GSF must continue to use timestamp, accepted/dropped output and real
  compositor timeline measurements as separate categories.
- The most useful next optimisation targets are fewer full-resolution
  reconstruction pixels, fewer full-screen copies, and refusing optional
  GPU work when the scene consumes the budget. The quarter-flow economy
  shader alone cannot remove these other costs.

### New opt-in completed-GPU-time admission

`GSF_NEXT_GPU_BUDGET_US=1500` (sample value, not a universal
recommendation) enables `gpu_cost_guard.h`. Default `0` leaves existing
runtime unchanged. When active:

1. Enable Vulkan timestamp collection, never infer GPU cost from wall
   clock or a multiply-derived "Output FPS".
2. Admit initially **one** synthetic output per eligible real pair, with measured ×3 promotion only when affordable, so
   economy-first load reduction takes priority over nominal x3/x4.
3. After first admission, await the **completed GPU sample associated
   with a positive synthetic count** before issuing another probe.
   Unavailable timestamps fail closed, preventing runaway probes.
4. If that completed pair exceeded budget, skip seven source-pair
   opportunities before another single synthetic probe. Two consecutive affordable ×2 samples permit a cautious ×3 probe only when the measured cost has sufficient headroom. A completed affordable two-output sample confirms ×3; over-budget work resets it. Original source presentation is never gated.
5. No deeper queuing, fabricated rate, or unbounded catch-up frames.

This is a deliberately conservative **P0 experiment**, and NOT proof
of a physical x2 frame rate or a complete Alpha+Owner game runtime.
It requires hardware A/B evidence to set or automate any default
budget: game native FPS retained, timestamped pair cost, actual WSI
presents/drops, and latency at p95/p99.

## ×2/×3 priority: compact owner lanes + measured promotion (2026-10-20)

Per developer priority, the **experimental one-layer Alpha owner bridge**
now accepts only multiplier ×2 or ×3. Normal game-facing GSF Next layer
remains unchanged and its old ×4 tests are retained as historical coverage.

- **×2:** `slots` physical synthetic targets: exactly one per work slot.
- **×3:** `2*slots` physical targets: exactly two per slot, with an
  Alpha batched pass processing both temporal phases in one full-resolution
  dispatch where the device supports the batch pipeline. The descriptor
  set still has three storage bindings for the existing shader ABI, but
  the unused third binding aliases the second real target and the shader
  never writes it for `count=2`.
- The private owner sets an internal `owner_target_stride`; normal WSI
  Alpha image indexing and three-output ×4 batching are unchanged.
- The batch recorder validates `count <= stride`, preventing an
  accidental ×4 work request from overwriting a neighbouring work slot.
- These savings apply to the owner's **output image allocation** only,
  not to all GPU memory. At two slots, six full-size synthetic textures
  become two at ×2 or four at ×3; the separate input snapshot/history
  and scene-change buffers remain allocated.
- `GSF_NEXT_GPU_BUDGET_US` is still **opt-in and disabled by default**.
  Its timestamp-based guard now probes single-output ×2 first. It only
  attempts two-output ×3 after two affordable x2 measurements and a
  conservative x2-cost extrapolation fitting the budget; observed
  affordable x3 timestamps confirm continued admission. Any measured
  overrun triggers a seven-source-opportunity cooldown and demotes
  immediately. No new optional GPU work is queued without receiving a
  completed positive-output timestamp from the last admitted probe.
- The Vulkan descriptor regression inspects both parity sets and all
  two-slot bindings to confirm no third image allocation, plus recorder
  validation denies a 3-output request in a 2-lane owner.

**Not yet achieved:** the Vulkan producer runtime still needs its real
single-layer native Output Owner swapchain transport wired end-to-end to
actual compositor WSI. A green shader or descriptor test is not a Deck
OLED FPS measurement. The strict historic ×4 output-pacing experiment
remains RED, independently from the x2/x3 work.

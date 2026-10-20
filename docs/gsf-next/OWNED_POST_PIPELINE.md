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

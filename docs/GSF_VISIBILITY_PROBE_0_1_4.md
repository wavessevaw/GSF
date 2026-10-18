# GSF Decky Test 0.1.4: bounded real-WSI visibility probe

## Purpose

Decky 0.1.3 protects native-source cadence when GPU synthesis cannot meet a strict deadline. On a Deck run that accepts zero synthetic WSI presents, that protection alone does not demonstrate a functioning in-game generator.

This is a **diagnostic test build**, not a 2x performance claim. Enable GFG_NATIVE_VISIBILITY_PROBE=1 only in the Decky test launcher. Retain the source-priority worker, real-frame FIFO, original semaphore consumption, queue synchronization, protected image ownership and fail-open fallback.

## Algorithm

* On each twelfth source pair, after history warm-up, consider **G1 only**, provided the frame clock has an intermediate display slot, the game is not overspeed and no source backlog is present.
* Admit that one output even when a stale predicted compute-cost or elapsed handoff deadline would reject it. Do not admit on an unsupported swapchain, a nonexistent intermediate slot or a failed source-history pair.
* Wait for the **real TE timeline completion**, with a hard 12ms ceiling, checking source pressure in 0.5ms slices. No fabricated pixels, no source-frame duplication for success.
* Attempt to acquire a real swapchain image within 2ms. A failed acquire or unfinished GPU task is a **drop**, never an accepted output.
* During this probe, hand off accepted generated content immediately (no stale GOOGLE desired present time). Forward the original real frame immediately afterward, with its original waits and metadata.
* Only successful downstream vkQueuePresentKHR on a generated image increments generated_wsi_accepted. This is **not** physical scanout or a promise of visibly distinct motion.
* Log each attempted probe as visibility_probe with synthetic_wsi, real_wsi and the actual drop_detail. Without proof of actual accepted generated WSI, the UI must not say generation was confirmed.

## Limits / known hazards

* An actual GPU task submitted before a readiness timeout cannot be preempted; occasional frame-time stalls and GPU contention remain possible. The probe is sparse, not always-on.
* Waiting for one generated slot in the worker can delay the corresponding original; the probe has a 12ms readiness + 2ms image-acquire budget. Native cadence preservation in a saturated game is not proven.
* vkQueueWaitIdle / vkDeviceWaitIdle still drain the worker to honor API semantics. This build intentionally does **not** apply an unsafe timeout=0 replacement.
* Do not remove the Vulkan image/semaphore lifetime guard or worker waiting-image synchronization merely to increase output counts.
* The game's limiter, TDP and target FPS stay unchanged. 90Hz OLED remains the primary test. An unsupported DXVK WSI surface must still fail open.
* The previous extracted-software-Vulkan acceptance checks still require distinct actual generated pixels and at least 60 generated WSI outputs in their 5Hz controlled test. A real Deck game can still legitimately accept zero.

## Required qualification before calling this successful

1. CI: native compilation, sanitized unit tests, Decky TypeScript/Python tests and extracted offline launcher, exact candidate source/commit checks, actual Vulkan software-driver pixel correctness and live WSI artifact.
2. Real Deck: check that **generated_wsi_accepted > 0**, game log contains visibility_probe synthetic_wsi=1, and native source cadence remains acceptable versus plugin-off baseline.
3. Separately verify actual scanout/visual distinctness. WSI-accepted means the driver accepted a generated vkQueuePresent, nothing stronger.

If generated remains zero, inspect: GSF_REAL_WSI_GATE, gpu-bridge startup, visibility_probe detail, output-worker pressure/stale, GPU readiness, WSI-acquire-unavailable and the JSONL per-slot drop_detail. Do not report a fake 2x win.

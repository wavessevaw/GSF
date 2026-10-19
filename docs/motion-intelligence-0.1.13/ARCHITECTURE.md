# GSF Motion Intelligence 0.1.13 Experimental

Base: `experiment/flow-lite-10w`, PR #49, `fab10095b72a9e07caf1962aa0a54f65fce51c9f`.
This is an experimental implementation and compatibility-limited preview. It is
**not completion of every acceptance criterion in the technical specification**.
No stable installation is replaced by this work.

## Multi-frame path

`frame_clock.h` reserves up to three synthetic slots on the observed refresh grid.
The real endpoint has its own index 3, separate from G1/G2/G3 (0/1/2).
45 FPS / 90 Hz x4 admits one synthetic, 30 / 90 x4 admits two, and
22.5 / 90 x4 admits three. Phase is `(slot - planned A) / (planned B - planned A)`.
Expired slots are removed before TE dispatch; outputs are compacted without
changing their phase. Tests cover fractional display/source relationships,
source jitter, overspeed and refresh changes. A selected x4 stays x4 when
admission or quality is reduced. Effective WSI multiplier is reported separately.

Command buffers, acquire semaphores, imported destination images, GPU estimates,
slot readiness/drop records, pacing plans, external layout setup and teardown
all have three synthetic entries. Timeline blocks use four values: capture,
G1/G2/G3 (or a final history-only completion). Deferred G2/G3 have separate
completion signals; cancellation retires the final value without reconstructing
cancelled outputs. Watcher records are marked submitted per output, so a skipped
timeline value is not evidence that its corresponding image was generated.

## Independent buffered output

`buffered_output.c` is linked into the normal TE-enabled layer; it does not use
`test_loader_output_runtime.h`, global Vulkan loader calls, CPU pixels or test
registration. All calls use the borrowed device's next-layer dispatch. Queue
operations share the layer's host serialization with application submissions.

The app sees distinct offscreen source VkImages. A capture-retirement worker
waits their copy fences and releases app indices immediately after snapshots
finish. A separate output worker retains private snapshots through generation
and transport completion, acquires from one real WSI and submits G/real output.
There are eight snapshot slots. Only GPU-ready, unstarted obsolete jobs can be
reclaimed. Executing or capturing slots cannot be overwritten. If every snapshot
is occupied, the application's waits are consumed once, its source is retired
without claiming a synthetic, and the overload drop is counted. Output is bounded.
App Present does not wait for generation, readiness or output WSI.

Buffer 0 adds no retention beyond the intrinsic causal A/B lookahead and
supports interpolation. Buffer 1 adds one source period, Buffer 2 adds two.
Additional delay is rounded to whole display slots, preserving the refresh
lattice for fractional source/display ratios. Both endpoint coordinates shift;
phases still come from the reserved actual slot. All three modes retain source
images through their consumers and pace real output even when a synthetic expires.
The configured buffer latency is additional retention, not input-to-photon latency.

### Supported owner contract and fallback

Independent output is explicitly enabled with:

```
GFG_NATIVE_BUFFERED_OUTPUT=1
GFG_NATIVE_SOURCE_CONTRACT=barriers
GFG_NATIVE_BUFFER_DEPTH=1
```

The source contract is SDR8, one swapchain, exclusive sharing, one layer, FIFO,
no create/present pNext extensions, and explicit image barriers (including sync2).
The normal Decky launch does not assert that arbitrary games satisfy this
contract. Buffer selection is persisted and applies when the independent owner
is active; runtime diagnostics distinguish ordinary WSI from the owner.

Unsupported create contracts retain ordinary WSI before a virtual handle is
published. A virtual handle is never forwarded to downstream create/destroy,
acquire, present or extension queries. Unsupported late presentation metadata
returns OUT_OF_DATE and requests ordinary recreation; its waits are not replayed.
Recreation drains/stops the old owner. Device-lost errors quarantine resources.

Remaining qualification work includes legacy renderpass/framebuffer layout
virtualization, present-ID/fence translation on the *virtual* source, maintenance
extensions/dynamic modes, HDR/packed10 independent output, Gamescope-injected
metadata and driver-specific recovery. Existing ordinary WSI retains its ID,
fence, pNext, dynamic mode and packed10 paths. The owner still uses the application's
queue and performs additional GPU copies; host independence is not proof of
GPU scheduling independence or preservation of native FPS.

## Hierarchical Flow Lite 2.0

Shared-flow density 6 selects the new path. Density 5 remains Flow Lite 1.0;
compact shared-flow 0 remains the ECO45 control; GPU LMC remains available.
Pyramids, block motion, temporal prediction, compact confidence and HUD counters
are computed once per source pair. Search is at 1/16 and 1/8 resolution, with
1/4 refinement in Balanced/Quality. Ultra Lite omits that refinement and uses a
smaller search radius. No full-resolution motion search is introduced.

Each time coefficient reconstructs an RGBA16F image at half source resolution
(640x400 for 1280x800), then performs one full-resolution upscale/repair pass.
Sparse RGB correction is shared, not repeated per synthetic. Unreliable or
implausibly large vectors use conservative temporal blending, which can ghost.
Local edge/confidence weighting avoids a global blur. Persistent HUD/static
pixels are restored from original source pixels after scene reconstruction.
The consistency/visibility test is approximate, not a full independent backward
flow solution. HUD separation is conservative and cannot guarantee perfect UI.

Both sequential cheap reconstruction and a z-batched dispatch are implemented.
`GFG_NATIVE_FL2_BATCH=1` selects the latter; it shares the same shader body and
writes distinct output images using each output's coefficient. It remains opt-in:
software measurements cannot choose the more efficient RDNA2 implementation.

Profiles: Ultra Lite, Balanced, Quality, Adaptive. Adaptation changes quality
using measured GPU execution cost (intentional deferred presentation gaps excluded) with a bounded upgrade interval, retaining
the selected multiplier. No TDP/GPU-clock modification or full-frame CPU transfer
is performed. The default is Balanced.

## Telemetry semantics

`slot_mapping` correlates A/B IDs, requested/applied tau, compact destination
index and timeline signal. Schema-2 `synthetic_lifecycle` records all three
slot states independently. `gpu_output` records GPU render, low-resolution
reconstruction and upscale/repair timestamps. Legacy schema-1 pair arrays keep
G1/G2/real ordering for compatibility; use schema-2 events for G3.

Live diagnostics distinguish generated FPS (GPU-completed reconstructions, excluding holds; retirement sampling can lag),
WSI-accepted output FPS, source FPS, selected/effective multiplier, GPU generation
span and buffer estimates. GPU-ready observations are separate from dispatch.
An accepted WSI handoff is not physical display evidence. Displayed remains null.
Bandwidth, hardware queue waiting and physical unique/repeated frames require
independent instruments; no estimates are presented as measured values.

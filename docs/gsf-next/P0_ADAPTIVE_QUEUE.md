# Queue-depth research: failed proportional Auto hypothesis

**Status: rollback/containment after Steam Deck feedback, 2026-10-20.**
Original proportional Auto in PR #59 has **not** passed real-device acceptance.
The user found an intermediate queue depth that temporarily stabilised output;
further increases made stability worse, while ×2 never sustained a true 2×
output uplift and ×3/×4 delivered no corresponding gain.

## What is actually deployed on this experimental branch

The Decky profile retains `max_in_flight=0`, now presented as
**Защитный режим · 2 кадра**. It requests a constant **2 jobs**, regardless
of Hz or multiplier; it is not a feedback-driven optimum finder.

Manual 1..8 remains only to reproduce and diagnose the observed response
curve. Manual 4..8 may add explicit playout hold `effective_queue+1`
display ticks up to 9 ticks / 100ms. Auto and manual 1..3 preserve the
previous hold policy. Actual queue is always limited by swapchain image
count and allocated GPU ring capacity. The private output ring may still
allocate 3..8 slots × 3 synthetic images (with a ~256 MiB estimate), but
**ring capacity is not permission to queue a frame**.

The old Auto policy `min(8,max(2,ceil(hz*multiplier/90)+1))` is
discontinued, not optimized. Its old 90 Hz ×2/×3/×4 depths of 3/4/5
and 120 Hz ×4 depth of 7 were unsupported extrapolations. Extra queued
images cannot repair a GPU producer that processes fewer output
frames per second than the compositor consumes.

## Root-cause target

A single game WSI swapchain still couples generation, output and real image
lifetime. The generator waits for GPU reconstruction before releasing the
real source in the ordinary path; higher queues therefore trade more
latency and game-image backpressure for an apparent short-term gain.
The experimental early-ingest option removes one fence dependency but
still shares queue service and WSI output ownership.

New architecture: [single-owner post-game output transport](OWNED_POST_PIPELINE.md).
Reuse the prior Vulkan virtual source / output owner prototypes from
`engine/gfg-native` and qualify with actual games and Gamescope before
enabling. No output multiplier can exceed the *physical* refresh ceiling.

## Tests and measurement

- `gsf_queue_depth` verifies Auto stays at 2 over 40..240 Hz and ×2..×4
  and manual caps/clamps work; mandatory CI now executes it.
- Decky profile/transport covers setting 0..8; Vulkan matrix tests manual
  depth 8 and the conservative Auto at 144 Hz.
- Real Steam Deck A/B must measure game-source FPS relative to Off,
  accepted **real and unique synthetic** WSI frames, output interval
  p50/p95/p99, source stall time and slope of queue age during a long run.
  Existing CI on Lavapipe cannot claim actual 90 Hz or panel scanout.

**Acceptance:** stop publishing new 'pacing fixes' based only on enlarging
queues. Do not claim 2×/3× until a new output transport demonstrates sustained
real-device throughput and temporal uniqueness. Full standalone/Gamescope
interoperability and the private output owner are pending.

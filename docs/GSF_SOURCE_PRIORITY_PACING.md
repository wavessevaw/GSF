# Source priority and pacing attribution (Decky 0.1.3)

The original single-pending mailbox parked the application Present call while
the worker could wait for GPU generation, a busy transport ring or a future
handoff. A CPU worker did not isolate those waits from the game or its GPU.

## Final behavior

* Original presents use an ordered FIFO bounded by swapchain image count
  (maximum 64), plus one executing call. No real job is overwritten. Capacity
  overflow conservatively drains before synchronous fallback; it is recorded,
  not disguised as a dropped synthetic frame.
* A newly submitted original announces pressure **before** consuming its app
  semaphores through queue submission. The worker can yield even if the caller
  is waiting for the queue's required host serialization.
* With queued originals, skip optional history/generation and present the
  original with its private gate and immutable metadata. Busy ring fences are
  polled; failure to reserve a ring falls back without waiting on a fence.
* Generation admission rechecks pressure before dispatch. An already submitted
  workload cannot be canceled, but synthetic readiness/handoff waits yield
  within 0.5 ms polling slices when a new source arrives. Timed GOOGLE real
  presents use immediate desired time under pressure, preserving their IDs.
* Private gates still consume application waits before Present returns.
  Per-image pending **counts** retain the semaphore-reuse guard even when a
  reacquired image is resubmitted before the preceding CPU job retires.
* JSONL no longer calls fflush for every pair and feedback batch. Flush at
  250 ms intervals and at close; buffered records may be lost on a hard kill.

This is not a promise of a nonblocking Vulkan driver. Required queue host
serialization, driver WSI calls, unavailable swapchain images and already
queued GPU work can still stall. On a shared saturated GPU, removing a CPU
wait does not remove LMC's GPU cost. The last-interval clock and limited
generation deadline budget remain separate limitations.

## Automatic checks

`output-worker` freezes its backend and submits three real frames. Submission
of frame three must complete while the backend remains stopped (one-second
watchdog), application waits must already be consumed, IDs must retire in
FIFO order, and pressure must be visible inside the submit callback. It also
checks immutable caller memory, per-image semaphore lifetime, stale history,
bounded drain and deferred errors. `gpu-ring` tests pre-dispatch pressure and
zero-output history signaling. `presentation-timing` interrupts a target two
seconds away and verifies immediate GOOGLE time and batched flush boundaries.
These tests pass locally with ASan/UBSan; they are not GPU performance tests.

`tests/pacing_ab.py` runs actual Vulkan/LMC with validation on an extracted
candidate. Five configurations: layer off, source-only, compute-only, full,
and the old full implementation at commit
`61037766aef41703668d8029a54303e5f1e0da71`. Only the latter's native library is
changed; the test application and Temporal Engine are identical. Each has
steady 45Hz and variable 30..45Hz profiles with short bursts and a 120ms scene
gap, detailed JSONL off/on, and two repetitions in reversed order: 40 runs.
No test changes a real game's limiter or display configuration.

The source-only lab control forwards real presents through the same worker
without source-copy/LMC work. Compute-only dispatches LMC without synthetic
readiness/acquisition/presentation and without planned real handoff pacing.
Both use `GFG_NATIVE_DIAGNOSTIC_MODE` and are not exposed as product modes.
The ordinary game launcher never sets this variable.

The runner asserts original-frame counts, skipped/presented accounting,
validation success, zero host pixel reads, no capacity fallback in the new
implementation, and actual compute dispatch in compute-only mode. It archives
per-run logs, JSONL, source rate, Acquire/Present p50/p95, native output counts,
and a summary. Timings exclude the first three source samples. No FPS or
speedup threshold is used to conceal results on a slow software device.

Other existing checks still require distinct generated pixels, swapchain
recreation/resize, exact extracted plugin install/restore and WSI ordering.
Software Vulkan results do not qualify Steam Deck performance or unique
physical scanout. Hardware qualification should follow these automated
checks, rather than ask the owner to diagnose each intermediate change.

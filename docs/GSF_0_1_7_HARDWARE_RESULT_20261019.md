# 0.1.7 hardware result and the next controlled experiment

The `(5)` game archive ran the correct 0.1.7 test merge
`0ee3a9bdca2b9070bcf723d853b378168d0afe00`, with calibrated timestamps and
latency admission enabled. All 197 GPU/correlation records join one process
and bridge without ambiguity. There were no watcher expiry, submission-failure
or device-error records. Exit code `-2` is SIGINT, not proof of a Vulkan crash.

## What worked and what did not

| Observation | 0.1.6 archive `(4)` | 0.1.7 archive `(5)` |
| --- | ---: | ---: |
| Capture duration | 146.541 s | 208.403 s |
| Source-pair records | 8,034 | 10,510 |
| Synthetic dispatched / WSI accepted, whole capture | 1,412 / 197 | 197 / 85 |
| Dispatch rate after first 60 s | 9.568/s | 0.492/s |
| Median submit → GPU start after first 60 s | 41.749 ms | 41.258 ms |
| Median GPU span after first 60 s | 8.739 ms | 8.021 ms |
| Accepted synthetic after first 60 s | 0 | 0 |

In `(5)`, the minimum observed recovery spacing after the first minute is
2.000316 s and the median is 2.035 s. This confirms the bounded-retry policy.
All 85 accepted synthetic frames precede 9.269 s. The game portion of this
capture therefore does **not** demonstrate a working multiplier or recovery.

Observed source-presentation rates over the post-60s portion are 42.385/s and
43.528/s respectively. Scenes, routes and durations are not matched A/B data;
do not claim this difference as a causal FPS improvement. The global GPU
sampler remains at a median 99%; it does not identify which process owns work.

The global `(5)` submit-delay median of 1.587ms mixes many fast opening samples
with sparse later retries. It does not mean loaded-scene latency fell from
35.7ms to 1.6ms. Comparisons must retain phase and sampling-rate information.
Similarly, a cumulative generated counter of 85 proves historical WSI handoffs,
not continuing generation throughout the run.

## Updated interpretation

Excessive optional dispatch was a confirmed admission defect and is now
suppressed. The roughly 41ms start delay persists with about one synthetic job
per two seconds. This weakens the hypothesis that an accumulating queue of
GSF interpolation jobs alone explains the delay; it does not identify the
remaining dependency or establish a pure queue wait.

One hypothesis is that CPU `vkQueuePresentKHR` timestamps precede actual GPU
source-image readiness. The clock's potential intermediate deadline can then
expire before source copy/interpolation can start, despite acceptable producer
throughput. Game GPU work, source-copy dependencies, queue scheduling and WSI
must be distinguished before changing presentation policy. Neither moving the
deadline blindly nor reducing LMC work alone is proven to solve this capture.
`gfg_frame_clock_pair` is deliberately unchanged.

## Autonomous render-readiness lab

The normal Vulkan test application now has an opt-in delayed producer
dependency: `GFG_TEST_RENDER_READY_DELAY_MS`, with optional `FROM`/`UNTIL` frame
limits. A host timer releases an application-owned `VkEvent`, and its render
command buffer waits before transfer. Render-finished binary signals are still
submitted normally. This uses the host-event synchronization documented by
[vkCmdWaitEvents](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWaitEvents.html)
and [vkSetEvent](https://docs.vulkan.org/refpages/latest/refpages/source/vkSetEvent.html).
It introduces no production-layer host signaling, fake GPU readiness or shader
delay. Events are reset only after the owning application's render fence;
the helper thread is joined after draining GPU work.

`render_readiness_lab.py` runs healthy generation, layer-off delayed rendering,
old and new admission with the same delay, and removal of the dependency to
test recovery. It requires actual asynchronous submission (an app-side submit
stall cannot stand in for measured GPU-start latency), calibrated GPU samples,
preserved originals, zero bridge host pixel reads/waits and clean Vulkan
validation. A failure remains a failure, with logs and machine-readable data
uploaded by the dedicated CI workflow in both layer orders.

The injected wait is **not GPU saturation or a measured cause on Deck**. A
successful reproduction establishes a controlled failure mechanism for further
timing work. A new candidate should follow evidence of how source readiness and
WSI handoff affect the deadline, rather than another unqualified guard change.

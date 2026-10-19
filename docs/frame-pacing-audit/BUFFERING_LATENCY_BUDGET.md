# Causal budget and added latency

All CPU scheduling coordinates use CLOCK_MONOTONIC. Source timestamp means
CPU Vulkan Present entry, not input GPU-ready time. Calibrated submit→start
includes dependencies and command delivery; it is not a hardware queue-depth
measurement. No hardware queue cause is inferred from an outstanding fence.

At 45→90, a source pair spans about22.222ms. The G slot is11.111ms before B.
With a quarter-refresh handoff lead, the ordinary remaining readiness window
at source B is about8.333ms. Measured median x3 submit→GPU-start41.682ms plus
7ms illustrative compute already exceeds that window by40.349ms, before copy,
acquire and uncertainty. Faster shaders alone cannot recover every such pair.
The same archive's x2 median start7.513ms is a different observation: with7ms
compute it also cannot fit every8.333ms window. These are retained-segment
statistics, not an attribution to TDP, driver scheduling or every scene.

The reference forecast sums queue-start estimate, cumulative per-output compute,
copy, acquire, lead and calibration uncertainty. The stress model explicitly
varies start delay0.1/7/15/35/50ms and compute5/7/10/15ms; unexpected completion
spikes still expire. Live admission currently measures TE compute/start and
uses its conservative margin/lead. It does NOT yet measure every copy/acquire
GPU component. Readiness/acquire cancellation is still required, and a live
fully conservative budget is an open gate; model arithmetic is not substituted
for those missing measurements.

Strict real-priority mode adds no planned B extension. It forwards native B
when no synthetic qualifies. This safely declines impossible slots; it cannot
guarantee continuous multiplication when the causal budget is exceeded.

The explicitly configured reference buffer shifts BOTH A/B coordinates and
slots BEFORE phase calculation. For a41ms start delay +7ms compute +0.2ms copy
+0.1ms acquire +0.01ms uncertainty, four90Hz refreshes (44.444ms) fit the modeled
G deadline; zero added buffer does not. The target τ remains0.5. The configured
extra B SCHEDULE delay relative to the same planned B is exactly44.444ms. With
a baseline B target22.222ms after source entry, the shifted B target is66.667ms
after source entry. Neither number is measured input→display latency or measured
B delay against a real no-GSF game run. They must not be advertised that way.

No buffered mode is exposed in the plugin because the direct WSI worker lacks
the independent resource/pipeline lifetime needed to honor it under source
pressure. The prototype validates an alternative transport, not this latency
contract. Measured B-over-baseline p50/p95/p99 and maximum input→display latency
remain UNMEASURED until matched hardware runs and independent scanout proof.

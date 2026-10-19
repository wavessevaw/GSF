# Verification and preliminary performance report

Environment: Ubuntu 22.04 container, Mesa 23.2.1 lavapipe/llvmpipe (LLVM 15), two software render threads. No `/dev/dri`, Steam Deck, Gamescope scanout or power sensor is attached. The user requested a build and hardware protocol instead of SSH hardware testing. Software timing is **not RDNA2 performance evidence**.

## Completed software gates

- Flow Lite 2.0 full Vulkan regression: **197 checks, 0 failures**, independently for sequential and batched reconstruction. Existing quality thresholds were retained. Includes coloured camera motion, HUD, scene cuts, duplicate/hold behavior, timeline protocol, source lifetime, three distinct G1/G2/G3 images and GPU-ready timestamps.
- Real Vulkan WSI: x2/x3/x4, all output counts observed, slot-derived phases and ordered accepted handoffs; validation errors 0. Performance warnings recommend TRANSFER layouts instead of GENERAL, and are not hidden.
- Buffer 0/1/2: independent owner active, 12 source presents each, bounded ownership, no app Present GPU waits. Buffer 0 dispatched **zero** synthetic images: this is a documented limitation, not successful x4 interpolation.
- Packed10/FP16 x4: all 1024 code levels preserved for the supported A2B10G10R10 format, three outputs. Linear-light PQ x4 agrees with the CPU oracle. A2R10G10B10 BLIT_DST is unsupported by this software device and explicitly skipped. This does not qualify HDR independent WSI.
- Virtual WSI bootstrap/dispatch regression passed, including borrowed/test-owned lifetime distinction. Native clock/phase, transport-ring, presentation timing, fallback and queue serialization tests passed.
- Decky: TypeScript check, frontend build, Node tests, panel interaction tests, 35 Python checks. The packaged extraction/install/GPU acceptance test is recorded separately after ZIP construction.

## Sequential comparison: GPU timestamps, milliseconds

Input is a deterministic 256x160 pan/object texture with a static HUD, 12 source pairs per configuration. Discard the first pair: 11 retained samples. Each backend sees identical source pairs and canonical t=1/2, 1/3+2/3 or 1/4+1/2+3/4. No CPU pixel readback is included in the benchmark timed GPU pipeline. The small sample makes p95/p99 effectively a maximum, not a tail-latency qualification. Runs are sequential in a shared cloud host; CPU contention and compiler/cache effects remain possible.

| Backend | Mode | Shared analysis p50 | Pair p50 | Pair p95 | Pair p99 | Cost/synthetic p50 | Allocation MiB |
|---|---|---:|---:|---:|---:|---:|---:|
| GPU LMC | x2 | 152.452 | 164.061 | 345.054 | 345.054 | 164.061 | 2.36 |
| GPU LMC | x3 | 114.124 | 135.104 | 154.954 | 154.954 | 67.552 | 2.68 |
| GPU LMC | x4 | 129.177 | 159.687 | 244.824 | 244.824 | 53.229 | 2.99 |
| ECO45 | x2 | 17.431 | 23.264 | 34.570 | 34.570 | 23.264 | 2.01 |
| ECO45 | x3 | 21.064 | 33.528 | 419.806 | 419.806 | 16.764 | 2.33 |
| ECO45 | x4 | 16.763 | 40.651 | 62.271 | 62.271 | 13.550 | 2.64 |
| Flow Lite 1.0 | x2 | 5.374 | 7.943 | 33.710 | 33.710 | 7.943 | 2.01 |
| Flow Lite 1.0 | x3 | 4.779 | 11.112 | 12.607 | 12.607 | 5.556 | 2.33 |
| Flow Lite 1.0 | x4 | 3.576 | 11.720 | 25.159 | 25.159 | 3.907 | 2.64 |
| Flow Lite 2.0 | x2 | 6.408 | 9.180 | 13.982 | 13.982 | 9.180 | 2.09 |
| Flow Lite 2.0 | x3 | 5.082 | 9.573 | 16.171 | 16.171 | 4.787 | 2.48 |
| Flow Lite 2.0 | x4 | 4.640 | 12.215 | 14.983 | 14.983 | 4.072 | 2.87 |

These measurements support shared analysis amortization on this software workload, not a claim of 90 FPS on Deck. Flow Lite 1.0 retains its original adverse chroma/cut regression failures when that old variant is selected; the new variant passes the unchanged gates.

## Per-output and batching comparison

| Flow Lite 2.0 | Dispatch | G1 p50 | G2 p50 | G3 p50 | Pair p50 | Pair p95 |
|---|---|---:|---:|---:|---:|---:|
| x2 | separate | 3.115 | — | — | 9.180 | 13.982 |
| x2 | batched | 2.253 | — | — | 7.578 | 15.355 |
| x3 | separate | 2.287 | 2.072 | — | 9.573 | 16.171 |
| x3 | batched | 2.240 | 1.601 | — | 8.382 | 9.886 |
| x4 | separate | 2.649 | 2.298 | 2.032 | 12.215 | 14.983 |
| x4 | batched | 3.935 | 2.277 | 2.586 | 14.231 | 22.376 |

The batched launch reconstructs all low-resolution images in G1 before per-output upscale, so its G1 cost includes that shared batch. Separate dispatch remains the default: x4 was faster here, and choosing on RDNA2 still requires target measurements. This is not an unconditional batching optimization.

## Buffer observations (software WSI)

| Buffer | Source presents | Dispatched synthetic | Accepted synthetic | Mean worker source age ms | Peak snapshot occupancy |
|---|---:|---:|---:|---:|---:|
| 0 | 12 | 0 | 0 | 2.377 | 1 |
| 1 | 12 | 32 | 32 | 189.999 | 2 |
| 2 | 12 | 32 | 32 | 374.285 | 3 |

Worker source age is a CPU-monotonic observation through WSI handoff; it is **not measured input-to-photon latency**. Synthetic dispatch and acceptance differ, and Buffer 2 is not assumed better just because it holds more data. These slow-source software tests do not prove the benefit of buffering at 45/30/22.5 FPS.

## Full-resolution functional probe

A separate 1280x800 x4 Vulkan run produced three outputs from one pair through
640x400 reconstruction and full-resolution upscale, with validation errors 0.
This single cold software sample measured shared analysis 8329.363 ms
and output GPU spans 176.490, 74.489, 58.478 ms.
It proves the allocation/dispatch path at the requested dimensions, not frame
quality, stable p95/p99 or Steam Deck performance.

## Remaining release gates

- General Virtual Source WSI compatibility: renderpass/framebuffer layouts, virtual source present IDs/fences, maintenance/dynamic extensions, HDR output owner and actual Gamescope/Proton games. Ordinary WSI has the prior compatibility path; independent output is barrier-only opt-in.
- Complete interpolation in Buffer 0. Currently there are no synthetic frames when the zero-lookahead path has no causal slot.
- Source FPS degradation versus game baseline, hardware bandwidth/queue waiting, GPU clock, utilization, power/TDP, input-to-photon latency, actual displayed/unique frames, RDNA2 batching selection and full-resolution quality/performance. They are **not measured**, not zero.
- Steam Deck OLED 45→90, 30→90, 22.5→90 at 15 W and 10 W/AUTO. No physical FPS/power result is claimed.

The experimental ZIP and draft PR are reviewable implementation artifacts. They do not satisfy the full technical specification or authorize replacement of the stable version. Follow [the hardware protocol](DECK_PROTOCOL.ru.md) before qualification.

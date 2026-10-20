# Alpha 5 software efficiency experiment

**lavapipe/llvmpipe LLVM 15 on a CPU, not RDNA2. No physical FPS, power or input-latency qualification.**

Baseline: Alpha 4 `c92ea2919a9bccdca46ea7f14180be4e7e3bc0a1`. Candidate: the Alpha 5 direct-ring and shared-pixel reconstruction implementation. Ubuntu 22.04/GCC 11.4, Xvfb/X11, half-resolution flow/five levels, full-resolution reconstruction, two lavapipe worker threads. The environment has a 2-CPU quota (`200000/100000`); scheduler/throttling noise affects software GPU timestamps.

Each mode has two interleaved runs of 48 source frames. The first 16 frames are excluded. Only pairs that generated all requested intermediates enter the cost percentiles; the complete-pair coverage is reported explicitly. Partially admitted pairs stay in raw logs but are excluded from the comparison. This is a conditional GPU-cost comparison, **not proof of complete multi-frame throughput**. No validation or pixel readbacks run during timing. The separate correctness matrix validates pixels and ownership.

| Extent | Mode | Path | Pair p50 / p95 / p99, ms | Reconstruction p50, ms | Scratch copy p50, ms | Complete / warm pairs |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 640x480 | ×2 | baseline | 29.208 / 51.035 / 58.602 | 9.240 | 0.248 | 64 / 64 |
| 640x480 | ×2 | copy | 27.504 / 37.048 / 44.820 | 8.359 | 0.284 | 64 / 64 |
| 640x480 | ×2 | direct | 28.875 / 46.065 / 50.998 | 9.460 | 0.000 | 64 / 64 |
| 640x480 | ×2 | batch | 32.538 / 102.638 / 218.400 | 10.523 | 0.000 | 60 / 64 |
| 640x480 | ×3 | baseline | 49.678 / 62.232 / 117.971 | 24.118 | 0.449 | 64 / 64 |
| 640x480 | ×3 | copy | 45.153 / 54.849 / 60.523 | 20.587 | 0.392 | 64 / 64 |
| 640x480 | ×3 | direct | 46.204 / 58.217 / 78.255 | 22.250 | 0.000 | 64 / 64 |
| 640x480 | ×3 | batch | 33.571 / 61.899 / 91.421 | 14.954 | 0.000 | 64 / 64 |
| 640x480 | ×4 | baseline | 44.088 / 62.940 / 64.174 | 25.290 | 0.694 | 64 / 64 |
| 640x480 | ×4 | copy | 57.740 / 115.520 / 126.723 | 33.099 | 0.766 | 64 / 64 |
| 640x480 | ×4 | direct | 58.809 / 67.998 / 70.669 | 34.521 | 0.000 | 64 / 64 |
| 640x480 | ×4 | batch | 42.877 / 52.617 / 61.810 | 21.357 | 0.000 | 64 / 64 |
| 1280x800 | ×2 | baseline | 84.959 / 144.498 / 190.860 | 27.596 | 0.782 | 62 / 64 |
| 1280x800 | ×2 | copy | 90.501 / 131.712 / 169.708 | 31.644 | 0.880 | 63 / 64 |
| 1280x800 | ×2 | direct | 97.194 / 121.207 / 134.044 | 35.423 | 0.000 | 64 / 64 |
| 1280x800 | ×2 | batch | 78.064 / 102.347 / 175.765 | 26.656 | 0.000 | 63 / 64 |
| 1280x800 | ×3 | baseline | 153.991 / 193.308 / 220.308 | 77.112 | 1.779 | 61 / 64 |
| 1280x800 | ×3 | copy | 106.130 / 130.784 / 182.927 | 54.799 | 1.760 | 62 / 64 |
| 1280x800 | ×3 | direct | 149.778 / 178.953 / 198.855 | 74.549 | 0.000 | 62 / 64 |
| 1280x800 | ×3 | batch | 98.551 / 142.081 / 155.498 | 46.347 | 0.000 | 64 / 64 |
| 1280x800 | ×4 | baseline | 149.696 / 175.614 / 292.006 | 93.064 | 2.626 | 62 / 64 |
| 1280x800 | ×4 | copy | 146.855 / 206.085 / 325.864 | 90.653 | 2.538 | 57 / 64 |
| 1280x800 | ×4 | direct | 137.563 / 199.522 / 262.865 | 86.464 | 0.000 | 58 / 64 |
| 1280x800 | ×4 | batch | 120.000 / 159.573 / 246.445 | 64.274 | 0.000 | 59 / 64 |

`baseline` is the untouched Alpha 4 binary. `copy` is Alpha 5 with its scratch-copy override. `direct` is the default independent direct warp. `batch` requests shared-pixel reconstruction; with ×2 it deliberately runs the exact same one-output direct warp, making it a useful noise control. Baseline scratch-copy time is the residual between pair/motion/warp intervals because the old report did not expose that field separately.

Results do **not** establish a uniform default-path speedup: some direct runs are slower than the baseline, and the ×2 direct/batch noise control differs although the reconstruction path is identical. At 1280×800 ×3 the batch reconstruction median was 46.347 ms versus 74.549 ms for independent direct reconstruction; at ×4 it was 64.274 versus 86.464 ms. These CPU-driver observations justify retaining the batch experiment, not enabling it unconditionally on RDNA2.

The default removes one scratch allocation and one full-image copy per synthetic. It preserves flow resolution, motion search, HUD criteria and reconstruction math. The memory-traffic reduction is structural; an actual GPU-time/energy gain is **unproven on Steam Deck**. The compatibility copy override permits a fair hardware A/B test. Batching stays opt-in until same-device measurements demonstrate a gain without source-FPS or image-quality regression.

Aggregate timestamps, coverage and run medians are retained in `efficiency-software.json`. Reproduce raw logs using `engine/gsf-next/tests/efficiency.py`; methodology and byte-count calculations are in ALPHA5_EFFICIENCY.md. A display-slot cap avoids knowingly generating excess output but does not prove scanout. The separate lifecycle benchmark records failed numeric throughput acceptance as failed, without weakening its thresholds.

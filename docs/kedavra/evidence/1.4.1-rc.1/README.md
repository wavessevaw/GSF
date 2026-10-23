# 1.4.1-rc.1 software evidence

Baseline: v1.4.0 (`6176caf141f2c908cea12ac7f22609cbd21eed33`). Candidate:
1.4.1-rc.1 scheduler and warp. All image tests use Mesa 25.0.7 lavapipe,
LLVM 19.1.7, at 1280×800. No physical Steam Deck or Steam Machine is available.

`pacing.json` contains three seeds × 180 seconds for each of five scenarios.
The comparison uses the production scheduler in both binaries, identical game,
analysis and presentation timing assumptions, and the unchanged pacing analyzer.
Hitches include freezes and jumps; short quarter-speed steps count as slowdowns,
not freezes. Median latency stays within 0.02 ms of baseline in this matrix.
This does not prove identical perceptual smoothness in a game.

`engine-baseline.json` and `engine-candidate.json` use the same instrumented test
host, with baseline or candidate shaders. `psnr_border` covers the outer 5% of
the shorter image dimension, using actual output pixels even for scaled images.
The baseline and candidate both pass existing SDR, PQ and scRGB quality gates.
The new border-specific regression is expected to fail against baseline shaders;
it is not included in the recorded baseline result.
Candidate border PSNR gains averaged over three phases: pan +0.749 dB, fast pan
+0.223 dB, dolly +0.206 dB, near object +0.139 dB. Static frames, cuts and the
objects/HUD scene are unchanged. Camera sequence temporal error falls from
5.2756 to 5.1867; worst-frame PSNR improves 23.207 → 23.381 dB.

Small regressions are retained transparently: slow-pan sequence PSNR
29.096 → 29.074 dB and temporal error 5.4329 → 5.4345; climb border PSNR
changes by −0.078 dB (full-frame −0.006 dB). Climb was already an open,
non-gating case. This is an overall improvement, not a claim that every pixel
or every scene improves.

The dedicated known-motion border test checks all four edges and 63 phases,
including continuous source weighting near the last valid texel centre.
Scheduler tests cover an unfinished analysis pair with a usable real endpoint,
and a short runway that previously ran out after two half-steps.

Reproduce from the repository root:

```sh
# Compile the old scheduler in a separate checkout of v1.4.0:
make -C /path/to/baseline/kedavra build/kfg_sim
make -C kedavra all
python3 kedavra/sched/compare_pacing.py \
  --baseline /path/to/baseline/kedavra/build/kfg_sim --out comparison.json
make -C kedavra test-pacing test-sched test-engine
python3 -m unittest discover -s kedavra/decky/tests -v
# Use the same current test host against both shader directories:
kedavra/build/kfg_vktest --spv /path/to/baseline/kedavra/build --json baseline.json
kedavra/build/kfg_vktest --spv kedavra/build --json candidate.json
# Add --lowbase for 24→90 Hz. For timings run without concurrent workloads:
kedavra/build/kfg_vktest --spv kedavra/build --scene static --bench 30
```

Sandboxed local sanitizer runs use `ASAN_OPTIONS=detect_leaks=0` because LSan
cannot run under the environment's tracing; ASan and UBSan stay enabled. CI uses
the unmodified sanitizer settings. The 16-scenario matrix (44 seed/scenario runs),
400 scheduler fuzz runs, 29 Decky backend tests and filter tests pass locally.

The plan adds no GPU pass, image allocation, history frame or analysis resolution.
Duplicate candidate reads are skipped; slow/static pixels avoid the blur square
root. Actual hardware frame cost is unverified: compare `gpu_us_avg`,
`gpu_analysis_us_avg`, base-game FPS and a Pace trace on the same game/settings
before adopting this prerelease. Software timings are not hardware predictions.

## Base 24 / 90 Hz

`engine-lowbase-new.json` passes all checks, including the new border regression.
`engine-lowbase-baseline.json` passes the pre-existing gates but intentionally
fails 32 of the new border checks, demonstrating that the test detects the old
behaviour. Border PSNR gains: pan +0.394 dB, fast pan +0.347 dB, occlusion
+0.191 dB. Camera temporal error 5.8463 → 5.7497, worst-frame PSNR
22.933 → 23.102 dB. Small losses: dolly full frame −0.067 dB / border
−0.117 dB; near-object full frame −0.006 dB. No threshold was relaxed.

## Software timing

Two alternating baseline/candidate runs, 30 repetitions each, no other test
workloads, `LP_NUM_THREADS=4`. `--scene static` skips the long quality suite;
the benchmark itself always measures the existing objects scene. Times are
Vulkan timestamps from lavapipe on this shared CPU, not Deck GPU measurements.

| Run | Baseline analysis / two outputs (ms) | Candidate analysis / two outputs (ms) |
|---|---|---|
| 1 | 78.378 / 61.745 | 80.784 / 64.256 |
| 2 | 84.345 / 63.999 | 76.663 / 58.701 |

The two-output means are 62.872 → 61.479 ms (−2.2%), but the variation is larger
than the difference and includes an unchanged analysis stage. Treat this as no
clear software cost regression, not evidence of a hardware speedup. Raw logs
are included. Earlier concurrent timings were excluded because simultaneous
quality tests distorted CPU-driver timings.

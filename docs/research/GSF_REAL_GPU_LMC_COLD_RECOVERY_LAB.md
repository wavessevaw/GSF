# GSF real GPU LMC: autonomous strict pixel correctness at slow source cadence

2026-10-18. This test complements the existing X11 Output Owner lab, whose A/B split is NOT GSF LMC.

## Scope

- Actual full GSF Temporal Engine LMC GPU shaders, application-device Vulkan compute, native Vulkan implicit layer, real XCB Vulkan WSI and lavapipe, **not** a frame-split surrogate.
- Fresh process, deliberately slow 5Hz source input of 12 source frames (to give software Vulkan time to recover after a cold first dispatch). With 3x generation there are 22 possible synthetic slots.
- Tests both Vulkan Loader layer orders (real WSI handles; unlike the known unsupported private virtual handle case, issue #14).
- The application's real GPU smoke now strictly asserts: **all 12 real accepted; all 22 actual LMC-generated synthetic WSI accepts; zero skipped**, at least one positively verified pair, all checked pixels distinct; no Vulkan validation/bridge errors, no CPU waits or host readbacks on the present path. This is a correctness test at **5Hz source**, NOT real-time 45→90 performance.
- Separately asserts those properties in a Python JSON reporter. Logs are artifacts even on red.

## Critical distinction: slow-paced strict correctness vs actual 45→90 timing

**Evidence from first run**: [GitHub CI 39110541743](https://github.com/wavessevaw/GSF/actions/runs/39110541743) produced **12/12 real, 22/22 synthetic, 0 skips and 5/5 distinct pixel checks** for **both** layer orders, with no Vulkan errors. The runner was allowed source input at 5Hz. Following that result, the test was **strengthened**: the ALLOW_SKIPS flag was removed and both the built-in smoke and the JSON gate now require exactly zero skips and 22 accepted synthetic frames, without ignoring prior frames.

The inherited zero-skip, 8 real +14 synthetic, no pixel regression / same-core strict test is still unchanged and **red** in [draft PR #4](https://github.com/wavessevaw/GSF/pull/4). First-dispatch profiling in [draft PR #6](https://github.com/wavessevaw/GSF/pull/6) found cold first output ~21s, later output ~67-74ms in lavapipe, which is not Steam Deck RADV performance.

Two acceptance categories are deliberately independent:

1. **Paced source correctness PASS:** 12/12 real, 22/22 generated accepts, **0 skipped**, positive verified distinct LMC pixel pairs and Vulkan validation clean at deliberately paced 5Hz source. This tests the real shader rather than A/B split.
2. **Real-time 45→90 deadlines / full strict CI PASS (future):** the unpaced real-source scenario in draft #4 still must yield 8 real +14 generated, **0 skipped**, no validation errors and successful pixel checks. That has **not** been demonstrated. Slow 5Hz input does not establish 90Hz panel pacing or deadline behavior.

## Operation

[Real GPU LMC 5Hz GPU Correctness workflow](../../.github/workflows/gsf-native-lmc-cold-recovery.yml): daily at **03:41 UTC**, on native/Temporal source changes and manual dispatch. Two independent matrix jobs on GitHub Actions Ubuntu software GPU. Includes full build logs, raw native WSI GPU smoke stderr/stdout and machine-readable counter JSON retained 21 days.

Run standalone on x86_64 Ubuntu after building full Temporal engine and Native Vulkan layer with Khronos v1.4.365 headers:

    python3 engine/gfg-native/scripts/gsf_lmc_cold_recovery.py --order native-first --out result-native

**No Deck, no Gamescope, no physical OLED pixel proof.** A successful software GPU test does not prove 45-to-90 Deck timing. It only tests the **real LMC shader path** absent from the X11 A/B split output lab. GFG Extreme stable not touched.

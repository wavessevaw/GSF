# GSF real GPU LMC: automated cold-start recovery lane

2026-10-18. This test complements the existing X11 Output Owner lab, whose A/B split is NOT GSF LMC.

## Scope

- Actual full GSF Temporal Engine LMC GPU shaders, application-device Vulkan compute, native Vulkan implicit layer, real XCB Vulkan WSI and lavapipe, **not** a frame-split surrogate.
- Fresh process, deliberately slow 5Hz source input of 12 source frames (to give software Vulkan time to recover after a cold first dispatch). With 3x generation there are 22 possible synthetic slots.
- Tests both Vulkan Loader layer orders (real WSI handles; unlike the known unsupported private virtual handle case, issue #14).
- The application's own GPU smoke still asserts: **all 12 real accepted**; all synthetic slots accounted (accepted + skipped); positive pixel verification, all checked pixels distinct; no bridge errors, no host readbacks/CPU waits on the present hot path; clean Vulkan validation.
- Separately asserts those properties in a Python JSON reporter. Logs are artifacts even on red.

## Critical distinction: warm strict vs cold recovery

The cold test intentionally enables the existing **GFG_TEST_ALLOW_SKIPS=1** *only for this labeled recovery scenario*. Such skips may be correct when lavapipe first dispatch requires ~21 s and a real frame must take precedence. **This is not a green substitute for the strict original test.**

The inherited zero-skip, 8 real +14 synthetic, no pixel regression / same-core strict test is still unchanged and **red** in [draft PR #4](https://github.com/wavessevaw/GSF/pull/4). First-dispatch profiling in [draft PR #6](https://github.com/wavessevaw/GSF/pull/6) found cold first output ~21s, later output ~67-74ms in lavapipe, which is not Steam Deck RADV performance.

Two acceptance categories are deliberately independent:

1. **Cold recoverability PASS:** original real frames always survive; at least two genuine synthetic WSI accepts, positive pixel verification and Vulkan validation clean. Unexpected loss, no checked pixels or GPU errors are red; a cold start has classified synthetic skips.
2. **Warmed strict output PASS (future):** an explicitly delimited prewarmed measurement window that actually validates 8 real +14 synthetic, zero skips, positive pixel checks and legal synchronization; do not simply ignore prior drops. This step remains to be implemented correctly.

## Operation

[Real GPU LMC Cold Recovery workflow](../../.github/workflows/gsf-native-lmc-cold-recovery.yml): daily at **03:41 UTC**, on native/Temporal source changes and manual dispatch. Two independent matrix jobs on GitHub Actions Ubuntu software GPU. Includes full build logs, raw native WSI GPU smoke stderr/stdout and machine-readable counter JSON retained 21 days.

Run standalone on x86_64 Ubuntu after building full Temporal engine and Native Vulkan layer with Khronos v1.4.365 headers:

    python3 engine/gfg-native/scripts/gsf_lmc_cold_recovery.py --order native-first --out result-native

**No Deck, no Gamescope, no physical OLED pixel proof.** A successful software GPU test does not prove 45-to-90 Deck timing. It only tests the **real LMC shader path** absent from the X11 A/B split output lab. GFG Extreme stable not touched.

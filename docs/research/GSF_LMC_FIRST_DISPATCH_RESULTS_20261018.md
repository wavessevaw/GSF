# GSF Native first-frame profile: measured lavapipe results

**Date:** 2026-10-18. **Code:** [GSF stacked PR #6](https://github.com/wavessevaw/GSF/pull/6), on top of the [strict regression gate #4](https://github.com/wavessevaw/GSF/pull/4). **Run:** [39103085665](https://github.com/wavessevaw/GSF/actions/runs/39103085665). **Environment:** Mesa lavapipe CPU Vulkan / Xvfb, 96×64 SDR test, 8 real frames, asynchronous GPU LMC `lmc-gpu`, 3×, both Vulkan layer orders, verification every second pair. NO Steam Deck/RADV performance claim.

This experiment adds `GSF_TE_PROFILE_INIT=1` to log Vulkan host-call times during context construction, without changing shaders, synchronisation, frame cadence or the acceptance assertions.

## Measured evidence

| Measurement | Native layer first | Vulkan validation layer first |
| --- | ---: | ---: |
| Temporal Engine pipeline creation total (host) | 202.141 ms | 225.643 ms |
| LMC context creation total (host) | 1,155.271 ms | 1,020.111 ms |
| Initial GPU initialization submit + wait | 0.490 ms | 0.197 ms |
| Total context creation (host) | 1,378.922 ms | 1,399.734 ms |
| Largest individual generic LMC compute pipeline (pipeline 10, GPU IRLS) | 245.371 ms | 253.037 ms |
| Search specialization pipeline creation (8 variants) | roughly 71–94 ms each | roughly 59–82 ms each |
| First actual LMC output readiness, from pair analysis | **21,054.310 ms** | **21,867.997 ms** |
| First actual motion-model GPU stage | 9,679.921 ms | 10,046.654 ms |
| Subsequent output readiness | ~67–74 ms | ~66–74 ms |
| Actual WSI-accepted synthetics | 6 | 10 |
| Skipped synthetics | 8 (history) | 4 (history) |
| Verified distinct and ordered pairs | 3 / 3 | 2 / 2 |
| WSI acquire-not-ready | 0 | 0 |
| Vulkan errors | 0 | 0 |
| Strict regression result | **FAIL** (expected) | **FAIL** (expected) |

**Cross-run compare:** the cold first actual LMC evaluation is around 15× the *entire* TE context creation cost, and more than 20,000 ms later than ordinary warmed evaluations. This excludes *initial host-side `vkCreateComputePipelines` cost alone* as an explanation. It does **not yet prove** whether lavapipe lazily compiles CPU executable code on first dispatch, executes the shader extremely slowly once, incurs job scheduling contention or suffers another first-submit effect.

## Consequences for engineering

1. **Do not prioritize application `VkPipelineCache` as a cure for 21s** based on these results; host pipeline creation costs only ~1.0–1.2 seconds for the LMC component in the tested configuration.
2. The first actual GPU-compute session requires instrumentation *after queue submit*: exact source pair start, shader dispatch start, first GPU timeline completion, LMC model stage, first GPU-to-present readiness. Current `gpu_pair` already records model time but not fully isolated first queue dispatch vs lazy compilation vs work.
3. **Do not remove `st.skipped_synthetic == 0`** from the original strict CI. The observed skips are classed `history`, with `Output Worker` stale jobs. In production, protecting real frames is correct; in CI, correctness and imposed scheduling deadlines require separate deterministic tests.
4. Build separate subtests: (A) slow startup with classified recovery (and mandatory real-frame preservation); (B) *prewarmed* strict 8-real / 14-synthetic / 0-skipped / positive pixel checker inside an explicitly delimited measurement window. Prewarm outside the measured sequence; do not merely throw away unwanted frame counts.
5. Compare cold/warm under isolated Mesa caches and lavapipe LLVM instrumentation; try a second Vulkan process with cache retained. **Do not use RADV env settings as lavapipe compiler evidence**.
6. On Steam Deck, use RADV GPU timestamps and 8-CU RDNA2 profiler; never extrapolate 96×64 software Vulkan speed to the OLED.

## Provenance and reproducibility

- [Raw CI job logs and uploaded artifact: GSF strict run 39103085665](https://github.com/wavessevaw/GSF/actions/runs/39103085665)
- [Profiler implementation and instructions](GSF_TE_LMC_INITIALIZATION_PROFILE_20261018.md)
- [Previous cold-start result before profiling](GSF_NATIVE_LMC_COLD_START_DIAGNOSIS_20261018.md) (the latter currently resides in stacked PR #4)
- [Main Mesa research audit](GSF_MESA_RADV_WSI_DEEP_AUDIT_20261018.md)
- Run `GSF_TE_PROFILE_INIT=1` in the software LMC smoke workflow, retaining `GFG_NATIVE_GPU_BRIDGE_VERIFY=1` and **no** `GFG_TEST_ALLOW_SKIPS`.

This is a diagnostic checkpoint only. No GSF hardware scanout evidence or stable 2×/3× release is claimed.

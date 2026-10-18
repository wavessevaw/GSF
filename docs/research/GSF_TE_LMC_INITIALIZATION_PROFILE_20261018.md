# GSF Temporal / GPU LMC cold-start breakdown

**Date:** 2026-10-18. **Status:** diagnostic code, not a GPU optimization or hardware performance claim.

This investigation follows the independent strict Native FG3 lavapipe CI restored in [GSF PR #4](https://github.com/wavessevaw/GSF/pull/4). It reproduces the inherited startup failure while the synthetic image checker separately verifies genuine interpolated pixel content.

## New opt-in native diagnostic

Set `GSF_TE_PROFILE_INIT=1` for **a single test process**, e.g.:

```bash
GSF_TE_PROFILE_INIT=1 GFG_NATIVE_FG=1 GFG_NATIVE_GPU_BRIDGE=1 \
GFG_NATIVE_ASYNC=1 GFG_NATIVE_GPU_SYNC=same-device GFG_NATIVE_ALGO=lmc-gpu \
GFG_TEST_EXPECT_GPU=1 GFG_TEST_VALIDATION=1 GFG_TEST_PROTON_CHAIN=1 \
GFG_TEST_FRAMES=8 GFG_NATIVE_GPU_BRIDGE_VERIFY=1 \
GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY=2 \
VK_INSTANCE_LAYERS=VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation \
VK_LAYER_PATH="$PWD/engine/gfg-native/build-triage:/usr/share/vulkan/explicit_layer.d" \
xvfb-run -a engine/gfg-native/build-triage/wsi-gpu-smoke 2>&1 | tee lmc-first-frame.log
```

The env flag is **off by default** and does not change GPU algorithms, queue scheduling, Vulkan present modes, drop policies, player settings or shader binaries.

### What new logs mean

Lines are machine-readable:

```text
[gfg-te] init_profile phase=lmc-shader-module pipeline=10 variant=0 host_ms=... result=0
[gfg-te] init_profile phase=lmc-compute-pipeline pipeline=10 variant=0 host_ms=... result=0
[gfg-te] init_profile phase=lmc-search-compute-pipeline pipeline=1 variant=0 host_ms=... result=0
[gfg-te] init_profile phase=temporal-pipelines-all pipeline=-1 variant=0 host_ms=... result=0
[gfg-te] init_profile phase=lmc-context-all pipeline=-1 variant=2 host_ms=... result=0
[gfg-te] init_profile phase=context-initial-gpu-submit-and-wait pipeline=-1 variant=0 host_ms=... result=0
[gfg-te] init_profile phase=context-total pipeline=-1 variant=2 host_ms=... result=0
```

- `host_ms` is a **CLOCK_MONOTONIC host-call duration**; it includes driver compilation / cache lookups as applicable but is **not measured GPU execution time**.
- `pipeline` for `lmc-compute-pipeline` is the `LMC_*` enum ordinal; the search pipeline uses `pipeline=level` (1–4) and `variant=grid` (0/1) for specialization constants.
- `temporal-shader-module` and `temporal-compute-pipeline` use pipeline=-1; each `temporal-pipeline-total` uses its `PIPE_*` ordinal, allowing timing of individual layers.
- `context-initial-gpu-submit-and-wait` includes Vulkan submission and wait for initialization only. It is **not** the first actual LMC motion-model evaluation.
- Context/pipeline totals **include** their nested child timings; do not sum totals together and double-count.
- Existing native logs `gpu_pair sequence=...`, `output1_ready_ms`, `model_ms`, `output-worker ... stale` and `gpu-verify ... distinct` then reveal first **actual pair** latency and whether the pixel checker succeeded.

### Interpretation criteria

1. If pipeline creation dominates, test **driver cache/pipeline cache** A/B separately; do not assume that a 13s first-frame delay is shader compilation without evidence.
2. If the context creation is fast but first LMC GPU model takes seconds, investigate shader dispatch, lavapipe CPU execution, and resources/timeline waits. The field `model_ms` may reflect GPU timeline work on **software lavapipe**.
3. If real WSI image acquires fail, prioritize swapchain image counts, ownership and synchronization; if `acquire_not_ready=0` and `errors=0` but `history>0`, distinguish stale generation history due to backlog from WSI failures.
4. Preserve **two** independent acceptance suites: expected classified cold start recovery, and a strict **warmed** 8-real / 14-synthetic / 0-skips / positive verified pixel test. Never simply delete `st.skipped_synthetic == 0`.
5. These are software CI findings. RDNA2 RADV performance, SteamOS Gamescope FIFO/MAILBOX routing, and **physical 90Hz OLED display** remain unverified.

### Next concrete work

- Run identical cold and warm 8-frame tests under isolated Mesa shader-cache directories and log hit/miss stats.
- Add bounded, deterministic LMC pipeline warmup **outside the measured acceptance window** without assuming the game render-finished semaphores can be delayed.
- If first dispatch is the source of delay, instrument per-GPU-stage timestamp correlation and establish a minimal warmup execution path on copied test frames.
- Keep source IDs, present timing, KMS/optical frames and real-frame latency separate from each other.

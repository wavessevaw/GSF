# GFG Temporal Engine (experimental)

Status: **ready for hardware trials, not verified on a Steam Deck.** Every
measurement in this repository comes from Mesa lavapipe (a CPU implementation
of Vulkan) or from operation counts in the reference implementation. None of
it is Steam Deck GPU time, power or latency. The engine is off by default and
never replaces the regular frame generator unless the player selects
*Experimental* and confirms it.

Benchmark report: [TEMPORAL_ENGINE_BENCHMARK.md](TEMPORAL_ENGINE_BENCHMARK.md).

## 1. Audit: where frame generation happens today

| Stage | Owner | Facts that shape this work |
|---|---|---|
| Real frame input | MAKO layer (`mako-render`) | Copies each presented image into one of two exported source images (RGBA8 or the native BGRA8/RGBA8 path of Frame Fusion). |
| Analysis + synthesis | MAKO backend (`mako-backend`, LSFG model) | Runs on its own `VkDevice`; shaders come from the user's `Lossless.dll`. The model is closed: it exposes no motion vectors, flow maps, confidence or per-region cost. `flow` is only a resolution divisor. |
| Contract | `backend::Instance::openContext` | 2 source FDs, N destination FDs (N ≤ 4), one shared timeline semaphore FD. The producer signals `idx` when a real frame is ready; output *i* signals `idx+1+i`; the next real frame is `idx+N+1`. |
| Presentation and pacing | MAKO layer + Frame OS | Generated frames of the pair (N−1, N) are presented before real frame N. Frame OS is the only pacing owner. |
| Policy | GFG Governor / Autopilot | One actuator trial at a time (arbiter), A-B verification, rollback, receipts. |

Consequences:

* Temporal reuse **cannot be added inside LSFG**: its work is one opaque model
  per output frame. The Temporal Engine is therefore a separate, open
  generator behind the same backend contract, selectable per context, with
  LSFG as the fallback for everything it refuses.
* Interpolation needs the *next* real frame. A generated frame between N−1 and
  N exists only after N was rendered, so frame generation adds at least one
  real-frame interval of latency. Higher output FPS is not lower latency, and
  nothing here claims it.
* A comparison against LSFG/MAKO quality or cost is **not possible in this
  environment**: the model ships only inside the user's licensed
  `Lossless.dll`, which is not available here and must not be redistributed.

## 2. Pipeline

```
real frame N ──► pyramid(N) ──► Temporal Analyzer ──► Frame Budget Scheduler ──► per output t:
 (B slot)        luma, 4 levels   (pair N-1, N, once)   (knobs per pair)           lists ─► REUSE / LIGHT / FULL ─► destination image
                 kept as next A                                                     (indirect dispatch, one executor per tile)
```

All of it is Vulkan compute (`engine/gfg-temporal/shaders`, GLSL 4.50 →
SPIR-V at build time). The NumPy reference
(`engine/gfg-temporal/reference/gfg_temporal_ref/core.py`) is the executable
specification; shaders follow it step by step and a parity test checks both on
the same frames.

### 2.1 Temporal Analyzer (once per real-frame pair)

* **Luma pyramid** (levels 0–3, 2×2 box). The newest frame's pyramid is kept
  and reused as the next pair's older frame: every real frame is converted
  once.
* **Bilateral hierarchical motion estimation** per tile (8, 16 or 32 px; 16 by
  default) on the *t = 0.5* grid: cost = mean |A(c+o−v/2) − B(c+o+v/2)| over a
  sub-sampled window, so one vector field serves every intermediate timestamp
  of the pair (*motion reuse*). Coarsest level: full search ±6 level px plus a
  temporal predictor (the previous pair's vector); finer levels: predictors
  from the coarser level and neighbours, then 3×3 refinement (half-pixel at
  level 0). A small bias toward short vectors keeps flat regions still.
* **Tile statistics**: match error E, zero-motion difference E0 (mean) and M0
  (max), vector smoothness S, neighbourhood motion, and a **probe residual** P:
  |A − B| along the *interpolated* field on a 4×4 grid per tile — what LIGHT
  would get wrong. P predicted the LIGHT/FULL quality gap far better than
  vector statistics in the tile study (see the benchmark report).
* **Scene cut**: more than half of the tiles with E and E0 above 20/255 →
  hold (nearest real frame) and history reset. Computed with two atomics, no
  CPU readback.
* **Confidence**: mean of `1 − P / cut_threshold` per tile.
* History holds only vectors, per-tile classes and counters. **Synthetic
  frames are never read back as a reference.**

### 2.2 Per-region classes

| Class | Executor | Cost per pixel (texture fetches) |
|---|---|---|
| REUSE | copy of the nearer real frame | 1 |
| LIGHT | two-sided warp along the bilinearly interpolated, time-corrected tile field | 2 + 8 buffer loads |
| FULL | per-pixel choice among the 9 neighbouring tile vectors and the interpolated field by a 3×3 bilateral luma cost at time *t*, then an occlusion-aware blend (nearer side where both sides disagree) | 180 + 2 |

* REUSE: E0 ≤ 1/255, M0 ≤ 16/255 and no motion larger than half a tile in
  the 3×3 neighbourhood (content passing through the tile would be lost).
* LIGHT: P ≤ 3/255 (× the scheduler's level scale).
* FULL: everything else.
* **Hysteresis** limits switching: an upgrade applies at once, a downgrade
  only after 2 consecutive eligible pairs.
* **Error feedback**: a LIGHT tile whose measured residual exceeds 6/255 is
  promoted to FULL for the remaining outputs of the pair and the next pair.
* **No seams by construction**: each tile is written by one executor; LIGHT
  and FULL share the same interpolated field (FULL prefers it through a
  smoothness bias), so neighbouring classes agree wherever motion is smooth.
  The seam metric in the benchmark measures this.
* There is no merge pass (tiles are written in place), so "merge cost" is 0.

### 2.3 Frame Budget Scheduler (`src/te_scheduler.c`)

Pure logic, unit-tested. Inputs are measured: stage times from timestamp
queries, class counts, real-frame interval, GPU busy share when known (the
backend does not know it today; it assumes 50 % and reports
`gpu_busy_known=false`).

* Budget = real interval × free GPU share × 0.85.
* Per-class tile costs are calibrated on a *profiled* pair every 32 pairs
  (executors serialised by barriers so each is timed alone); other pairs run
  without those barriers.
* Over budget, or 3 misses in a row → cheaper level (LIGHT threshold × 1.5,
  2.5, 4). 30 calm pairs (< 60 % of budget) → one level back.
* At the cheapest level the scheduler recommends fewer outputs, then fallback.
  **It never changes the number of presented frames itself**; the
  recommendation is reported for the pacing owner and Autopilot.
* **Analysis that does not pay is switched off**: when the work moved off FULL
  saves less than classification + list building for 8 profiled pairs, every
  tile runs FULL; this is re-evaluated after 240 pairs.

### 2.4 Cost cap (te.2)

The goal is **no more GPU time than the regular frame generator**. The
scheduler therefore bounds its budget by a hard cap per generated frame
(default 2 ms, `GFG_TEMPORAL_CAP_MS`). A pair measured over the cap moves up
one level immediately; the last level runs FULL tiles as LIGHT and disables
promotions. If the LIGHT-only path is still over the cap (for example when
analysis alone is too expensive), the adapter holds — outputs copy the newest
real frame and no analysis runs — for 120 pairs, then measures again. Whether
2 ms is at or below LSFG on a Deck must be confirmed by an Off/Experimental
comparison in the same scene.

Since te.2 motion estimation runs one 64-thread workgroup per tile (8
candidates × 8 window rows at a time; the first minimum in candidate order
wins, so results equal the sequential reference) and FULL takes one decision
per 2×2 block (`full_density = 2`, experiment B 2×2). On lavapipe the
executors became 65–69 % cheaper while the analysis became slower (a CPU
emulator penalises barrier-parallel code); GPU behaviour must be measured on
hardware.

## 3. Native implementation (`engine/gfg-temporal/src/te_engine.c`)

* Vulkan 1.2, loaded at run time with `dlopen` (like MAKO inside the layer);
  no link-time dependency on the loader.
* Capability checks with a stated reason: compute queue, timeline semaphores,
  RGBA8 storage + linear filtering, R16F (or R32F) storage + linear filtering
  for the pyramid, external memory / semaphore FD when descriptors are
  imported, device UUID equal to the backend's GPU.
* All resources are allocated at context creation and reused; nothing is
  allocated per frame. Memory per context is accounted
  (`te_context_memory_bytes`, reported as VRAM).
* Tile lists are built on the GPU with atomics; executors use
  `vkCmdDispatchIndirect`, so the CPU never waits for classification.
* Two in-flight pairs at most; a non-blocking submit returns `VK_NOT_READY`
  instead of queueing (shadow mode drops the pair). In experimental mode the
  previous pair must complete within 250 ms, the same bound as the LSFG
  context, so work cannot pile up behind the game.
* The first frame of a context, or a frame after a skipped one, produces
  holds (copies of the new real frame) and still signals every output, so the
  consumer never waits for a value that will not come.
* Swapchain recreation: the renderer closes and reopens the backend context;
  the engine context is destroyed only when idle, otherwise retired by the
  existing MAKO retirement path (`waitForIdle` with a 250 ms bound).

## 4. Renderer integration (`engine/gfg-temporal/mako`, `engine/gfg-fusion/renderer.patch`)

* `backend::Context` becomes a polymorphic base; the LSFG `ContextImpl` is
  unchanged except for `override`.
* `GFG_TEMPORAL_ENGINE` is read when the backend instance is created (inside
  the layer's layer-disabling environment). The engine creates its own device
  on the backend's GPU (UUID match).
* `observe`: no GPU work; one status record per context says whether the
  engine could run and why not.
* `shadow`: the engine imports duplicates of the source and sync descriptors,
  waits on the same timeline values, **never signals**, writes only private
  images and drops pairs when busy. A 25 % duty cycle (runs of 16 measured
  pairs; `GFG_TEMPORAL_SHADOW_DUTY`) bounds its GPU cost. LSFG presents.
* `experimental`: for SDR8 contexts the engine replaces LSFG; any refusal
  (encoding, missing capability, import failure) keeps the LSFG context and
  records the reason.
* One owner of swapchain and presentation: the MAKO layer. The engine only
  fills the destination images the layer already owns.
* Status: `<runtime-state>/temporal/<pid>-<start ticks>-<context>.json`,
  replaced atomically about once a second, removed when the context closes.

Build: `scripts/build_frame_fusion.py` builds `libgfg-temporal.a`
(`make -C engine/gfg-temporal lib`), applies the pinned patch (SHA-256 in
`engine/gfg-fusion/source.json`) to MAKO `edd2946`, builds and runs the full
CTest suite (50 tests including `gfg-temporal-adapter`) and the Frame Fusion
pixel tests.

## 5. GFG integration

* Setting `temporal_engine` in `gfg-governor.json`: `off` (default),
  `observe`, `shadow`, `experimental`. Experimental stores an explicit consent
  and is dropped back to off without it. Applies from the next game start (a
  launch marker becomes `GFG_TEMPORAL_ENGINE`; Flatpak games never get it).
* RPC `set_governor_temporal_engine(mode, consent)`; status in
  `get_governor_status().temporal_engine`.
* UI: Settings → Diagnostics → *Temporal Engine · Experimental*: mode, a
  confirmation for Experimental, state, class shares, GPU time per pair,
  dispatched / GPU-completed, dropped pairs, fallback reason.
* **Autopilot**: while the engine measures or generates, no GPU clock,
  shading (VRS) or flow trial starts (`temporal-engine-measuring`), so engine
  costs never mix with a trial's effect. TDP keeps its normal controller.
  Autopilot does **not** switch between LSFG, the engine and no frame
  generation yet: the choice is made at context creation, and a live switch
  needs a renderer transition path plus hardware verification first. The
  scheduler's recommendation (`keep`, `fewer-outputs`, `fallback`) is shown
  and logged for that decision.

## 6. Telemetry and log export

`temporal_engine` in the Governor status and in every session-recorder
sample; a `temporal_engine` section in the log report with a finding.

| Fact | Source | Notes |
|---|---|---|
| requested mode | GFG setting | |
| applied | live engine context exists | separate from requested |
| outputs dispatched / GPU-completed | engine counters | completed ≤ dispatched is validated |
| presented | — | **not observable in the backend; always `null`** |
| real FPS | engine's real-frame interval | output FPS comes from the existing renderer diagnostics |
| analyzer / REUSE / LIGHT / FULL / lists / merge GPU ms | timestamp queries | per-class times only from profiled pairs |
| pair GPU time p50 / p95 / p99 | last 240 pairs | |
| class shares, confidence, scene cuts, promotions | engine | |
| dropped pairs, misses, level, recommendation | engine scheduler | |
| fallback reasons | status records | |
| VRAM | allocated by the engine context | |
| power, temperature | existing Governor sensors | no attribution to the engine without an A-B-A trial |
| measured effect | — | none claimed; needs the hardware A-B-A protocol below |

## 7. Experimental mechanisms

All are in the reference only (`Params`/`experiments`), off by default, and
evaluated in the benchmark report. None is promoted to the GPU engine.

| Letter | Mechanism | Status |
|---|---|---|
| A | checkerboard FULL decisions | evaluated (reference) |
| B | FULL decision density 1×1 / 2×1 / 1×2 / 2×2 (compute density, not RADV VRS) | evaluated (reference) |
| C | luma per pixel, chroma per 2×2 | evaluated; on RGBA8 a texel fetch carries all channels, so fetch counts cannot fall |
| D | LIGHT from the nearer real frame only | evaluated |
| E | motion-adaptive: sub-half-pixel LIGHT tiles reused | evaluated |
| F | dual stage: LIGHT first, FULL where LIGHT failed (one output layer) | evaluated |
| G | interleaved quality across outputs of a pair | evaluated |
| H | dynamic-resolution integration | not implementable in a post-process backend: the engine sees the swapchain image. When the game renders below the display resolution and Gamescope upscales, the engine already works at the lower size; cost scaling with resolution is measured instead |

## 8. Limitations

* **Not run on a Steam Deck.** lavapipe runs the real shaders on the CPU:
  correctness, parity and relative cost between modes are meaningful,
  absolute times are not. RADV behaviour (DCC, wave occupancy, texture
  cache) may change the ratios.
* On lavapipe `VK_KHR_external_semaphore_fd` is missing, so the renderer path
  that actually generates (shadow/experimental with an imported timeline) was
  exercised only up to the capability refusal; the engine's timeline protocol
  is tested in-process, the memory FD import across two devices is tested.
* Quality ground truth: a rasterised game-like scene rendered at 90 FPS
  (`tools/temporal_bench/render_game_scene.py`) and four real 60 FPS camera
  clips. No commercial game capture was available. Thresholds were tuned on
  a subset of the game scene; the camera clips were not used for tuning.
* LPIPS was not computed (model weights unavailable offline); PSNR-Y, SSIM-Y,
  temporal error and a seam metric are reported.
* No comparison with LSFG (closed model, licensed payload).
* Only SDR8; HDR contexts stay on LSFG.
* The GPU busy share is unknown inside the backend; the budget assumes 50 %.
* Occlusion handling takes the nearer frame, so disoccluded background next
  to a fast object can show the older frame's content for t < 0.5.
* Autopilot does not choose between generators at run time (see §5).

## 9. Hardware trial protocol (what "verified" requires)

1. Install a build of this branch; leave the setting Off and record a log
   (baseline).
2. *Observe*: check the Diagnostics panel shows the engine as eligible for
   the game (otherwise the reason).
3. *Shadow* at 30→90 and 30→60: record 10 minutes; check GPU ms per pair
   p50/p95/p99, drop rate, class shares, no renderer errors, and FPS unchanged
   versus baseline within noise.
4. *Experimental* with an A-B-A schedule (Off – Experimental – Off, ≥ 3 minutes
   each, same scene): compare real FPS, output FPS, frame-time p95/p99, GPU
   busy, power and temperature; inspect fast turns, fast objects, HUD/text,
   fine detail and scene cuts visually; check swapchain recreation (resolution
   change, Steam overlay) and long runs (≥ 30 minutes).
5. Only a measured, repeated A-B-A gain moves the engine toward a default.

## 10. Reproduce

```sh
# engine, native tests (validation layer on), reference + GPU parity
make -C engine/gfg-temporal all
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json engine/gfg-temporal/build/te-tests
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json python3 -m unittest discover -s engine/gfg-temporal/tests -p 'test_*.py'

# renderer with the adapter, full CTest + Frame Fusion pixel tests
python3 scripts/build_frame_fusion.py --work /tmp/fusion --headers <Vulkan-Headers v1.4.365>/include

# benchmark (see TEMPORAL_ENGINE_BENCHMARK.md for the data sets)
python3 tools/temporal_bench/render_game_scene.py --out data/game90
python3 tools/temporal_bench/extract_video.py clip.mp4 --out data/clip --width 1280
python3 tools/temporal_bench/run_bench.py --seq data/game90 --step 3 --work out --production-timing
python3 tools/temporal_bench/run_experiments.py --seq data/game90 --out out/experiments.json
python3 tools/temporal_bench/report_tables.py out/*.json
```

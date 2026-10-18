# GFG Temporal Engine: benchmark report

**Read first.** All GPU times below were measured with Vulkan timestamp
queries on **Mesa lavapipe**, which runs the real compute shaders on a 4-core
CPU. They show the relative cost of the engine's modes on identical work;
they are **not Steam Deck times** and say nothing about Deck power or
temperature. Operation counts come from the NumPy reference and count what
the algorithm executes, not what it costs on a GPU. No result here has been
measured on a Steam Deck. Raw results: [`temporal_bench_results/`](temporal_bench_results/).

## 1. Hypothesis and verdict

*Hypothesis*: per-region adaptive processing with temporal reuse lowers the
GPU cost of synthetic frames by at least 10 %.

| Comparison | Result |
|---|---|
| Adaptive vs the engine's own FULL-everywhere path, 30→90, game scene, no profiling barriers | **−55 % GPU time per pair** (264 vs 589 ms on lavapipe) at **+0.05 dB** PSNR-Y |
| Same, 30→60 on four real camera clips | −62…−66 % (101–115 vs 290–304 ms), −0.1 dB |
| Motion analysis once per pair instead of per output | −25 % of the adaptive pair at 30→90, −39 % at 18→90 (analysis 89 ms reused by 2 or 4 outputs) |
| Adaptive vs LSFG / MAKO | **not measurable here** (closed model in the licensed `Lossless.dll`, not available and not redistributable) |
| Adaptive on a Steam Deck | **not measured** |

The hypothesis holds against the engine's own uniform path on this CPU
implementation. Whether it holds against LSFG, and on the Deck GPU, is open
until the hardware protocol in [TEMPORAL_ENGINE.md](TEMPORAL_ENGINE.md#9-hardware-trial-protocol-what-verified-requires) is run.

## 2. Data and method

* **Game-like scene**, 1280×800, 90 FPS, 630 frames, rendered by
  `tools/temporal_bench/render_game_scene.py` (moderngl + llvmpipe, MSAA 4):
  procedural materials, near/far parallax, thin fence wires, a fast running
  character, a 4-blade fan (rotation is not linear motion), a 180° camera turn,
  a static view with a HUD whose text changes twice a second, and a hard cut
  to a second location. Segments: strafe, objects, fastturn, static_hud,
  scenecut. It is synthetic; it is not a commercial game capture.
* **Real footage**, four 60 FPS camera clips from intel-iot-devkit
  `sample-videos` (CC-BY 4.0), 181 frames each, scaled to 1280 wide: a moving
  bolt, produce on a belt, a store aisle, a person walking. Real sensor noise
  and compression; mostly static cameras.
* **Decimation**: keep every K-th frame as real input, regenerate the K−1
  frames in between, compare with the held-out frames. K = 3 (30→90), 2
  (45→90 and 30→60), 4 (22.5→90) and 5 (18→90). 20→90 is not an integer ratio
  of the 90 FPS source; 22.5→90 and 18→90 bracket it.
* **Metrics**: PSNR-Y, SSIM-Y (scikit-image, Gaussian 1.5), temporal error =
  mean |Δoutput − Δtruth| in luma code values (flicker/judder that the truth
  does not have), seam excess = extra gradient error on tile edges between
  different classes relative to same-class edges (code values; ~0 = no seam).
  LPIPS was not run (weights unavailable offline).
* **Methods**: repeat (nearest real frame), linear blend, TE light-all,
  TE full-all, TE adaptive (default). The experiment mechanisms ran in the
  reference on 8 consecutive pairs of each game segment.
* **Threshold tuning**: the LIGHT/FULL criterion and its threshold were
  chosen from a tile study on 6 pairs of the game scene (§6). The camera
  clips were not used for tuning.

## 3. Quality and cost overview

PSNR-Y in dB (mean over all generated frames); GPU ms per real-frame pair on
lavapipe with profiling barriers (the production path is a few percent
faster, see §4).

| Run | repeat | blend | TE light | TE full | TE adaptive |
|---|---|---|---|---|---|
| game 45→90 | 26.80 | 29.53 | – | 35.35 / 346 ms | **35.51 / 156 ms** |
| game 30→90 | 26.82 | 28.27 | 33.07 / 191 ms | 33.18 / 634 ms | **33.23 / 268 ms** |
| game 22.5→90 | 25.95 | 27.40 | – | 31.42 / 844 ms | **31.44 / 377 ms** |
| game 18→90 | 25.51 | 26.72 | – | 30.10 / 1113 ms | **30.11 / 525 ms** |
| bolt 30→60 | 34.12 | 37.74 | 45.25 / 108 ms | 45.23 / 302 ms | 45.13 / 115 ms |
| produce 30→60 | 32.13 | 35.41 | 48.32 / 112 ms | 48.21 / 299 ms | 48.07 / 101 ms |
| store aisle 30→60 | 42.82 | 46.82 | 48.71 / 104 ms | 48.58 / 290 ms | 48.43 / 104 ms |
| walking 30→60 | 35.31 | 39.48 | 47.57 / 101 ms | 47.69 / 304 ms | 47.59 / 103 ms |

### 3.1 Game scene, 30→90, per segment

PSNR-Y / SSIM-Y / temporal error:

| segment | pairs | repeat | blend | TE light | TE full | TE adaptive |
|---|---|---|---|---|---|---|
| strafe | 44 | 21.43 / 0.805 / 8.96 | 22.42 / 0.789 / 7.70 | 30.87 / 0.963 / 1.81 | 31.20 / 0.967 / 1.59 | 31.17 / 0.967 / 1.62 |
| objects | 44 | 26.51 / 0.909 / 3.95 | 28.14 / 0.912 / 3.26 | 32.65 / 0.967 / 1.84 | 32.60 / 0.968 / 1.77 | 32.62 / 0.968 / 1.79 |
| fastturn | 44 | 21.79 / 0.755 / 12.39 | 22.92 / 0.753 / 10.34 | 24.85 / 0.831 / 8.80 | 24.85 / 0.834 / 8.78 | 24.86 / 0.834 / 8.78 |
| static_hud | 44 | 35.59 / 0.993 / 0.32 | 37.45 / 0.993 / 0.29 | 39.87 / 0.995 / 0.22 | 39.93 / 0.996 / 0.20 | 40.13 / 0.996 / 0.20 |
| scenecut | 29 | 29.77 / 0.901 / 2.76 | 31.51 / 0.907 / 2.15 | 39.23 / 0.983 / 0.82 | 39.50 / 0.984 / 0.76 | 39.50 / 0.984 / 0.79 |

GPU time (lavapipe) and classes:

| segment | method | pair ms mean / p95 / p99 | analysis ms | REUSE / LIGHT / FULL | seam excess |
|---|---|---|---|---|---|
| strafe | full | 580 / 629 / 669 | 85 | 0 / 0 / 1.00 | 0 (no class edges) |
| strafe | adaptive | 321 / 362 / 379 | 86 | 0.32 / 0.26 / 0.42 | 0.13 |
| objects | full | 718 / 887 / 912 | 109 | 0 / 0 / 1.00 | 0 |
| objects | adaptive | 287 / 358 / 377 | 96 | 0.20 / 0.54 / 0.26 | 0.51 |
| fastturn | full | 660 / 845 / 866 | 101 | 0 / 0 / 1.00 | 0 |
| fastturn | adaptive | 362 / 564 / 577 | 90 | 0.12 / 0.40 / 0.48 | −0.55 |
| static_hud | full | 608 / 680 / 714 | 91 | 0 / 0 / 1.00 | 0 |
| static_hud | adaptive | 144 / 178 / 192 | 90 | 0.79 / 0.17 / 0.04 | 0.15 |
| scenecut | full | 586 / 668 / 678 | 85 | 0 / 0 / 1.00 | 0 |
| scenecut | adaptive | 204 / 245 / 273 | 93 | 0.15 / 0.74 / 0.11 | 0.44 |

Seam excess of the adaptive path stays below one code value (0.5/255 on
average); light-all, whose residual promotions create isolated FULL tiles,
shows 1.4–2.3. Per-segment tables of every run:
`python3 tools/temporal_bench/report_tables.py docs/temporal_bench_results/*.json`.

## 4. Cost structure

* **Production vs profiled**: without the per-executor barriers, 30→90
  adaptive costs 264 ms per pair and full 589 ms (−1.5 % and −7 % against the
  profiled runs). Per-class times come only from profiled pairs.
* **Per tile per output** (30→90 strafe, profiled): FULL ≈ 0.058 ms, LIGHT
  ≈ 0.007 ms, REUSE ≈ 0.002 ms. FULL is ~8× LIGHT on lavapipe; the reference
  counts 182 vs 2 texture fetches per pixel.
* **Analysis dominates the adaptive pair**: 85–100 ms of the 264 ms at 30→90
  (in the reference the coarse full search ±6 is 46 % of the analysis fetches:
  170 candidates × 32 samples × 4 000 tiles). This is the next optimisation
  target, together with F and B below.
* **Resolution** (top-left crops of the game scene, 40 pairs, production):

| crop | adaptive pair ms | full pair ms | analysis ms |
|---|---|---|---|
| 1280×800 | 368 | 635 | 97 |
| 960×600 | 179 | 381 | 52 |
| 640×400 | 80 | 177 | 31 |

  Cost follows the pixel count; crops differ in content, so class shares
  differ too. This is what H can offer from a post-process backend: running
  at the game's render size when Gamescope upscales.

## 5. Experimental mechanisms (reference, game scene 30→90, 8 pairs per segment)

Texture fetches per generated frame, analysis included (23.7 M of it), and
quality on the same 80 frames:

| config | PSNR-Y | SSIM-Y | temporal err | fetches / output | vs adaptive |
|---|---|---|---|---|---|
| adaptive (default) | 34.88 | 0.9715 | 1.40 | 64.8 M | – |
| full-all | 34.84 | 0.9717 | 1.37 | 210.8 M | +225 % |
| light-all (+promotion) | 34.68 | 0.9700 | 1.49 | 38.1 M | −41 % |
| A checkerboard | 34.88 | 0.9715 | 1.40 | 48.1 M | −26 % |
| B 2×1 | 34.87 | 0.9714 | 1.40 | 45.5 M | −30 % |
| B 1×2 | 34.86 | 0.9714 | 1.40 | 45.5 M | −30 % |
| **B 2×2** | 34.84 | 0.9713 | 1.40 | **35.8 M** | **−45 %** |
| C luma/chroma | 35.06 | 0.9719 | 1.39 | n/a | fetch count not comparable (RGBA8 texel carries all channels) |
| D nearer frame only | 34.67 | 0.9704 | 1.48 | 63.6 M | −2 %, worse quality |
| E motion-adaptive reuse | 34.81 | 0.9711 | 1.42 | 64.6 M | −0.3 %, worse quality |
| **F dual stage** | 34.88 | 0.9711 | 1.42 | **37.8 M** | **−42 %** |
| G interleaved | 34.58 | 0.9695 | 1.46 | 45.4 M | −30 %, worse quality and temporal error |
| H dynamic resolution | – | – | – | – | not implementable in the backend (§4) |

* **Promising (not promoted)**: F (classification by the actual LIGHT
  residual in the same frame) and B 2×2 (one FULL candidate decision per 2×2
  block) keep quality within 0.05 dB and cut fetches by 42–45 %. They are the
  candidates for the GPU engine, after a Deck measurement shows that fetch
  savings become time savings there.
* **Negative results**: D, E and G save little or lose quality; G adds
  temporal error (alternating quality is visible as flicker). C improved
  PSNR-Y by 0.18 dB (chroma averaged over 2×2), but its cost claim cannot be
  made with RGBA8 sources, so it is not counted as a saving.

## 6. Classifier study (why the probe residual)

On 6 pairs (48 000 tiles, 31 158 with motion), the per-tile gap between
LIGHT and FULL error was 0 for the median moving tile and above 1 code value
for 9 %. Vector statistics were poor predictors (the field's curvature median
was 3.5 px because of ambiguous matches in flat areas). The probe residual —
|A − B| along the interpolated field on 16 points per tile — was not:

| probe ≤ | LIGHT share of moving tiles | mean gap | p99 gap | share of the total LIGHT–FULL gap lost |
|---|---|---|---|---|
| 1/255 | 33 % | 0.035 | 0.98 | 3 % |
| 3/255 (default) | 56 % | 0.068 | 1.42 | 11 % |
| 6/255 | 72 % | 0.110 | 2.04 | 23 % |

## 7. Observations and negative results

* **FULL's advantage is local.** Light-all is within 0.1–0.3 dB of full-all in
  the means; FULL matters at motion boundaries and occlusions, which the
  adaptive path routes to FULL. Mean metrics understate local artefacts.
* **Fast turns stay hard** for every method (≈ 25 dB at 30→90, 22.8 dB at
  18→90): motion beyond the ±48 px coarse search and the temporal predictor
  is not found. A global-motion predictor is the obvious next step.
* **Lower real FPS degrades all methods**: 35.5 → 33.2 → 31.4 → 30.1 dB from
  45 to 18 real FPS; generated frames do not replace real ones.
* **Parity**: GPU and reference agree on 99.8 % of tile vectors (≤ 0.5 px)
  and classes on the game scene, and differ by 42–60 dB PSNR in the output
  (half-float luma and hardware bilinear weights).
* **Not visible in these numbers**: perceived smoothness, latency (one real
  frame interval is added by any interpolation), Deck GPU time, power.

## 8. Reproduce

```sh
make -C engine/gfg-temporal all
python3 tools/temporal_bench/render_game_scene.py --out data/game90
for c in bolt-multi-size-detection fruit-and-vegetable-detection store-aisle-detection worker-zone-detection; do
  python3 tools/temporal_bench/extract_video.py sample-videos/$c.mp4 --out data/real60/$c --start 5 --frames 181 --width 1280
done
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
python3 tools/temporal_bench/run_bench.py --seq data/game90 --step 3 --work out --modes adaptive,full,light --production-timing --name game_30to90
python3 tools/temporal_bench/run_bench.py --seq data/real60/<clip> --step 2 --work out --modes adaptive,full,light --name real_<clip>_30to60
python3 tools/temporal_bench/run_experiments.py --seq data/game90 --step 3 --pairs 8 --out out/experiments.json
python3 tools/temporal_bench/report_tables.py out/*.json
```

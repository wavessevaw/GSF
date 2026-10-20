# Alpha 5: reduce memory traffic before reducing image quality

Alpha 5 follows Alpha 4 (`c92ea2919a9bccdca46ea7f14180be4e7e3bc0a1`).
It keeps the bounded private output ring, absolute presentation clock and one
motion analysis per A/B pair. The game still owns downstream WSI images;
this change does not claim virtual-source completion or zero source FPS impact.

## Default: reconstruct directly into the private GPU ring

Alpha 4 dispatched a full-size warp into one shared scratch image and copied it
into each job's private output. Alpha 5 binds the private output itself as the
warp's storage image. The presenter still acquires its WSI image just in time
and copies the ready private image into it. That final copy is necessary for the
current detached-storage design; it has not been counted as an eliminated copy.

Source WSI views and output storage views now have separate arrays, descriptor
counts and indices. Nine outputs cover the three bounded job slots and their
three possible phases. Per-job fences and presenter retirement protect reuse.
Output views are destroyed before their backing images, including on failure
and live quality-context replacement. Adjacent output warps read the same history
and flow but write different images, so they do not need scratch-reuse barriers.
The flow-to-warp and warp-to-transfer dependencies remain.

Private images use the generator's raw-copy-compatible storage format. RGBA/BGRA
8-bit sRGB surfaces use RGBA8 UNORM storage and the existing byte-swizzle variant
where needed; sampling an sRGB image as linear storage is not attempted. Source
capture retains its previous encoded-byte semantics. Packed10/half-float shader
variants compile and validate; HDR colorspaces remain real-only and are not
qualified by the X11 SDR tests. WSI images do not need STORAGE usage for this path.

For RGBA8 at 1280×800 this removes a 4,096,000-byte scratch payload (plus allocation
padding) and one 4,096,000-byte read plus one equal write per synthetic. At 45
synthetics/second the eliminated **logical copy payload traffic** is 368.64 MB/s;
at 67.5/s it is 552.96 MB/s. These are byte counts, **not measured DRAM bandwidth**:
caches, tiling/compression and transaction amplification are device-dependent.
The nine-image ring remains bounded; it has not been doubled.

`GSF_NEXT_OUTPUT_COPY=1` forces the compatible scratch-copy path for A/B measurement.
This is a process-start diagnostic option, not a live profile field. In managed
Steam launch options put diagnostic overrides **after** the launcher's `--`, e.g.:

```sh
python3 /home/deck/.local/share/gsf-next/launch.py --appid APPID -- /usr/bin/env GSF_NEXT_OUTPUT_COPY=1 %command%
```

Substitute the actual game APPID. Placing a `GSF_NEXT_*` override before the
managed launcher will be sanitized. The same command position can pass
`GSF_NEXT_WARP_BATCH=1` for the batch experiment. Remove the override to restore
the default, and restart the game when changing either diagnostic option.

## Experiment: reconstruct multiple phases in one pixel invocation

`GSF_NEXT_WARP_BATCH=1` enables the batched path for two/three reserved outputs.
One invocation fetches the interpolated block flow and performs the conservative
static-HUD comparison once, then evaluates the same reconstruction function at
each reserved phase. It binds three fixed storage images; the count guard skips
stores and reconstruction for unreserved phases. All statically bound images
have valid GENERAL layouts even when only two outputs are requested.

The one-output case keeps the independent shader. No motion analyzer is repeated.
A scene-cut verdict skips unnecessary flow sampling in both paths. No global blur,
flow-resolution reduction, frame duplication or changed quality thresholds was
introduced. Batch shaders are compiled into GPU pipelines only when explicitly
enabled; failure of an optional batch variant falls back to independent warps.

Batching stays **opt-in** until RDNA2 measurements show an improvement. Sharing
texture work can reduce fetches but increase register pressure and lower occupancy.
A lavapipe result cannot select the best Steam Deck shader. The default remains
independent direct warps. GPU reports expose `batch_ms`, `scratch_copy_ms` and
`direct_output`; batch duration is reported as one measured duration, never divided
into invented G1/G2/G3 measurements. Independent `warp_ms` remains per-output.

## Reproducible experiment and qualification

Run serially on the same driver, resolution, scene and power policy:

```sh
LP_NUM_THREADS=2 python3 engine/gsf-next/tests/efficiency.py \
  --build engine/gsf-next/build --baseline /path/to/alpha4-build \
  --out artifacts/next-efficiency --frames 64 --repeats 3
```

The baseline build must contain its Alpha 4 library/manifest and `afmf_headless`.
The script interleaves baseline/copy/direct/batch, tests ×2/×3/×4 at 640×480 and
1280×800, discards the first sixteen source frames, requires complete output
counts, retains raw GPU timestamps/logs and reports p50/p95/p99 plus each run's
median. A 360-Hz **software admission budget** permits all phases; this is neither
a 360-Hz display nor a source/scanout throughput benchmark. Timing runs disable
validation/readback overhead; the separate full pixel/ownership matrix retains
Vulkan validation, unchanged ±2-pixel motion thresholds and real GPU readbacks.

The 23-case matrix covers both output paths at ×2/×3/×4, batch ×3/×4/HUD; direct/batched with synchronization validation and no profiling,
queue depths, both validation orders, camera pan, recreation, live settings,
source overlap and allocation/submission failures. Clock, lifecycle and hot
reconfiguration tests remain separate from numeric throughput qualification.
See EFFICIENCY_REPORT.md for measurements and DECK_PROTOCOL.ru.md for hardware
acceptance. Power, physical cadence, source FPS preservation, input latency and
satisfactory quality in games require a Steam Deck run. No 10/15-W or LSFG/MAKO
parity result is inferred from these software tests.

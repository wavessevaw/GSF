# HDR PQ reconstruction: implemented, software numerical checks passed

internal.8 extends the launch repairs in 2.1.5-gsf.7. It fixes a concrete color
error: averaging PQ code values does not average emitted luminance. The native
layer also previously discarded `imageColorSpace`; packed10 alone cannot identify
PQ. No architecture comparison or PSNR sweep was started for this change.

## Implementation

The swapchain's color space is retained through layer → GPU bridge → TE context.
Only HDR10 ST2084 on the supported packed10 formats enables PQ reconstruction.
Unknown transfers such as HLG fall back to real-only with an explicit policy
reason. The source primaries stay unchanged; there is no tone map, gamut change,
brightness setting or power-control change.

The existing source preparation pass decodes the newest source once using the
ST 2084 EOTF. A pair of RGBA16F images caches **absolute nits**, allowing hardware
bilinear filtering in linear light. Normalizing these caches to 0..1 would lose
very dark values to FP16 subnormal precision. Both generated timestamps use the
same cached sources; only the final reconstructed color is encoded back to PQ.
Motion/confidence decisions retain their existing coded-value domain.

The cache costs 16 bytes per image pixel across two sources: **15.625 MiB at
1280×800**. It is allocated only for PQ contexts. PQ shaders have a separate
descriptor layout; ordinary SDR shaders do not gain HDR work or bindings. GPU
support for the extra samplers and filterable cache is checked before creation.
Hold, HUD/cut endpoint copies and exact duplicate recovery retain original codes
instead of taking an unnecessary PQ round trip. Live status/startup diagnostics
record the color space, transfer and cache byte count.

## Evidence and its limits

`scripts/check_hdr_pq.py` runs the real Vulkan compute path. The CPU oracle uses
double-precision ST 2084 equations on the quantized source endpoints and compares
packed10 GPU results within one code. Black/near-black, 100–200 nit and
5000–10000 nit channels are tested at 1/3, 2/3 and 1/2. For one x3 fixture:

| Output | Correct linear-light codes | Incorrect code-average values |
|---|---|---|
| G1 at 1/3 | 12, 549, 979 | 7, 544, 973 |
| G2 at 2/3 | 18, 573, 1003 | 15, 568, 998 |

Linking that oracle against the previous core reproduces the incorrect column
and fails, as expected. The negative log is preserved. An early implementation
also moved some identical-frame values by one code; the duplicate fast path
fixed this, and that failed log is preserved separately.

The six final numerical/precision cases pass: GSF x3, GSF x2, GSF duplicate,
GPU LMC x3 and both legacy precision paths. Exact duplicate sources retain all
1024 code levels. The 31 legacy SPIR-V modules are **byte-identical** to the
pre-change build. The 17 full GSF pipeline checks and native contracts also pass.

The software ICD supports the A2B10 path used here; it reports A2R10 BLIT_DST
unsupported, and those format cases are explicitly recorded as skipped. This
does not establish A2R10 support on a Deck. The tests use direct Vulkan images,
not an HDR-advertising SteamOS/Gamescope display. There is no physical HDR
scanout, game-scene quality, OLED luminance calibration, HDR GPU cost, or 90 FPS
acceptance result. These remain **NOT TESTED**.

The cloud still has no connected Deck endpoint or DRM device. G1 transport
ordering and the missed renderer-cut case remain separate work; this color
correction does not claim to solve them or close the product acceptance goal.

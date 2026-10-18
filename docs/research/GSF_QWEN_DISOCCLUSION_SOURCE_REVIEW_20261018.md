# Review: Qwen Disocclusion Resolver versus actual GSF GPU LMC

Date: 2026-10-18. Status: research only, not a shader implementation or quality acceptance.
Source: Qwen second report supplied by project owner. Pin: GSF main 421668f940487fccb0d3e9eb66e134608d453e72.

## Decision

Approve further study of confidence-gated source selection, but do NOT copy the proposed GLSL, do NOT replace existing LMC, and do NOT add three additional full-screen passes without a controlled A/B test. The minimal experiment belongs inside the current trajectory select/compose architecture. Projective background completion is a separate later ablation.

## Critical coordinate-contract mismatch

GSF engine/gfg-temporal/shaders/lmc/lmc_common.glsl explicitly says all motion vectors represent **A to B**, stored in their own grid pixel units. Both fieldA and fieldB store A-to-B motion in HALF-resolution pixels, but anchored on different frames. They are RGBA16F half-res, xy motion, z matching cost.

Qwen's standard forward/backward formula F_AB(p) + F_BA(p + F_AB(p)) = 0 requires F_BA to actually be B-to-A. It is not the second native GSF field. Applying that formula unmodified would reject even perfect translation. Example: both GSF fields report (4, 0) half-pixels; correct disagreement = 0, Qwen's mistaken sum has magnitude 8.

Current GSF half-res trajectory geometry, with v in A-to-B half pixels and tau between 0 and 1:

    a = target_half - tau * v
    b = target_half + (1 - tau) * v
    validA = inside(a) and norm(fieldA(a).xy - v) < tolerance
    validB = inside(b) and norm(fieldB(b).xy - v) < tolerance
    gridDisagreement = norm(fieldA(a).xy - fieldB(b).xy)

Convert these values by 2 only when using full-resolution source samples. Preserve mixed valid/invalid cases and source frame IDs.

## Existing functionality confirmed in source

- lmc_select.comp selects per-output-tau mode=3 (both), mode=1 (A-only), mode=2 (B-only), mode=0 (no credible correspondence). It already tests fieldA/fieldB agreement and photo residual.
- lmc_compose.comp already performs full-resolution source selection, one-sided reads, nearest-real HUD copy, scene-cut recovery and an HDR/PQ-specific path.
- lmc_sf_compose.comp already has time-dependent visibility and candidate rescoring, separately for tau=1/3 and tau=2/3, but is EXPERIMENTAL; docs/fg3/SHARED_FLOW_PROTOTYPE.md reports a post-cut HUD ROI regression as large as -2.007 dB and other quality failures. It must not be enabled based on this proposal alone.
- lmc_common.glsl stores up to three homographies as camera/motion layers. They are not guaranteed per-pixel background/foreground labels.
- G.palette contains candidate vectors, not a categorical motionLabel texture; a direct comparison with LABEL_BACKGROUND=2.0 is fiction.
- te_lmc.inc stores the prior motion field in m->temporal, not prior source RGB T-2. A color T-2 experiment needs an extra VkImage, lifetime, barriers, invalidation and memory measurements.

## Immediate GLSL correctness problems in the submitted pseudocode

1. sampler2D(tex, textureQueryLevels(tex)>0 ? 0 : 0) is INVALID GLSL: the second argument must be a sampler object, not an integer. Current GSF already binds combined sampler2D srcA/srcB. See https://docs.vulkan.org/glsl/latest/chapters/operators.html
2. The example assumes two opposite-sign flow images not present in GSF. The identifier flowAtoT is also used semantically as target-to-source, inviting direction errors.
3. It expects H_TtoA/H_TtoB precomputed, but GSF has A-to-B projective hypotheses, potentially multiple scene planes. Deriving target-time warps and selecting true background are not trivial.
4. It always copies A for HUD, whereas GSF selects nearest real (A if tau<0.5 else B), and the shader ignores existing BGRA/HDR/PQ contracts.
5. Its local fallback reads unwarped source at target x, inviting doubled edges; existing one-sided or nearest-real fallback is safer.
6. The Qwen proposed output bindings and memory sizes do not reflect current 39-binding LMC layout, R32_UINT HUD image, RGBA16F half-fields and SSBO trajectory selections.

## Recommended minimum P1 design: preserve existing passes

A: frozen baseline current LMC_SELECT + LMC_COMPOSE for tau=1/3,1/2,2/3.
B: in a feature-gated experimental variant, compute continuous uncertainty from grid disagreement, field cost and inlier consistency at the existing HALF-res select stage; the fourth component of SEL.sel is currently zero and could carry a bounded confidence subject to verification. Keep exact mode semantics, HUD/cut behavior and one-sided decisions. Use existing conservative compose fallback when uncertain; no extra full-res maps by default.
C: only after B improves held-out disocclusion ROI without regressions, test guarded projective candidate reuse; require valid source pixels and independently validated plane membership. The first or largest fitted plane is not necessarily camera background.
D: only then assess a real T-2 color image cache, with scene-cut/resume/recreate invalidation.

## Test acceptance

Run: python3 engine/gfg-temporal/bench/test_disocclusion_confidence_contract.py . This pure-Python test validates direction/tau/modes, NOT shader correctness or optical output.

Before any runtime PR, compare A/B/C on existing docs/fg3/SHARED_FLOW_PROTOTYPE.md corpus plus occlusion, thin wires, fast camera, water, particles, translucent effects, dynamic HUD, post-cut and 30-to-90/45-to-90 clips. Report masked PSNR/SSIM and moving-edge ghosting (ground-truth ROIs), motion-compensated flicker, HUD ROI, duplicate/hold output counts, actual GPU timestamp/power/memory. Measure the same source, same timing, same hardware, with no oracle leakage. The prior HUD regression is a hard warning.

Treat Qwen's +1 dB, -20% ghosting and memory bandwidth figures as proposed experiment criteria, NOT measured speedup. Interpolated frames at tau=1/3 and tau=2/3 need their own visibility decision. A missing scene detail in both A and B cannot be faithfully reconstructed from those images alone.

Primary milestone remains physical 45 real to 90 distinct SDR frames on Deck OLED via Gamescope, while red Native FG3 regression PR #4 and first-dispatch profiling PR #6 are resolved. This is an optional quality P1 branch, not a substitute for P0 WSI validation.

## References

- https://github.com/wavessevaw/GSF/blob/main/engine/gfg-temporal/shaders/lmc/lmc_common.glsl
- https://github.com/wavessevaw/GSF/blob/main/engine/gfg-temporal/shaders/lmc/lmc_select.comp
- https://github.com/wavessevaw/GSF/blob/main/engine/gfg-temporal/shaders/lmc/lmc_compose.comp
- https://github.com/wavessevaw/GSF/blob/main/engine/gfg-temporal/shaders/lmc/lmc_sf_compose.comp
- https://github.com/wavessevaw/GSF/blob/main/docs/fg3/SHARED_FLOW_PROTOTYPE.md
- https://docs.vulkan.org/glsl/latest/chapters/operators.html
- https://openaccess.thecvf.com/content_CVPR_2019/html/Bao_Depth-Aware_Video_Frame_Interpolation_CVPR_2019_paper.html

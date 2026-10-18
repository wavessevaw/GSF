# Branch review and GPU diagnostic attribution

Review baseline: `main` at `bc83e0c785dcd56d53c4fb3220af8a88d95fb93b`.
All 29 topic branches and all 29 PR records were checked on 2026-10-18 UTC.

## Integration status

| Branch group | Main integration | Decision |
| --- | --- | --- |
| Migration, licensing, research, Output Owner CI, autonomous lab, cold recovery | PRs #1–3, #5, #7–10, #12–13, #15–19 merged | Continue from main |
| Deck kit, resize, layer order and Decky selection | PRs #20–22, #25–27 merged | Preserve fixes; some source branches differ after squash merges |
| DXVK formats, expired admission and source priority | PRs #28–30 merged | Preserve current production implementation |
| Visibility probe, timeline watcher and calibrated timestamps | PRs #31–33 merged | Current diagnostic baseline is 0.1.6 |
| `test/native-gpu-lmc-ci-20261018` | PR #4 remains a diagnostic draft | Do not merge its historical red cold-start workflow over current tests |
| `diagnostics/te-lmc-first-frame-20261018` | PR #6 remains a stacked diagnostic draft | Host pipeline profiling is already present in main; keep original evidence |

`git merge-base --is-ancestor` and `git cherry` were checked alongside PR merge
records. A divergent source branch is not evidence that its work was lost:
several changes were squash merged. All research branches are ancestors of main.
The active strict 5 Hz GPU test and cold/warm cache lab remain separate from
real-time Game Mode qualification; neither proves Deck 2× performance.

## Confirmed report defect

`pair.te_sequence` is the count of successful TE submissions in a bridge, not
proof that the current source pair submitted anything. A source-only or
GPU-budget bypass repeats that counter. The previous report indexed every pair
by this counter, replacing the original window with later bypass windows.
It also joined only by sequence, although sequences restart for every bridge
and process. `gpu_correlated` and timeline watcher records lacked this namespace.

The fix records an additive `te_submit_sequence` (zero on bypass/failure), adds
`pid` and `generation` to correlated/watcher events and indexes only pairs with
actual synthetic submissions. Legacy records fall back to `dispatched_outputs`
and are joined only when their available identity identifies one origin.
Ambiguous records are counted in `gpu_sample_joins` and excluded from window
medians. Global valid calibration medians do not require a window join.
The existing `te_sequence`, TE ABI and report fields retain their meanings.

## Hardware-log replay

The latest available game archive was recorded before PR #33: it contains no
`gpu_correlated` events. It cannot establish calibrated GPU start latency or
the cause of GPU contention in 0.1.6.

| Replay observation | Result |
| --- | --- |
| Parsed pair / GPU sample records | 6,775 / 1,248 |
| Incomplete JSON tail | 1 line ignored |
| Counter overwrite across a 10-second boundary | 3 |
| Generated submission whose original window changed | 1 |
| GPU sample joins after repair | 1,248 matched, 0 ambiguous, 0 unmatched |
| Synthetic submitted / WSI accepted | 1,249 / 212, unchanged |
| 70–80 s GPU-ready median | 9.676 → 9.680 ms |
| 80–90 s GPU-ready median | 9.668 → 9.659 ms |

This particular replay produces small median changes. The process/recreation
collision can misattribute larger amounts of data and is covered independently
by regression tests. The repair does not explain or fix the game's frame drops.

## Validation and next decision

Regression tests cover events emitted before their pair record, repeated
counters on bypasses, explicit zero submission, equal sequences across two
processes and recreated bridges, ambiguous legacy records and conflicting
duplicate origins. The extracted-package Vulkan smoke also checks that native
events and the installed Python reporter agree on submission identity.

No changes to `gfg_frame_clock_pair`, LMC, GPU budget policy, source priority,
queue ownership, readiness waits or visibility probe cadence are included.
COOLDOWN, Tiles and a separate queue remain deferred. The next scheduling
decision requires trustworthy 0.1.6 calibrated data and controlled calibration
on/off measurements; source FPS alone is not an admission rule.

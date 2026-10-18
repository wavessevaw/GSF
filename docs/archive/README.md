# Historical GFG research recovered during the branch review

These are **historical documents**, not current GSF implementation or device
acceptance results. Dates, instructions, network observations, open gates and
references to “current” software inside them describe their original snapshots.
They do not change the current development scope or enable old experiments.

The original extraction covered the Direct WSI snapshot. It did not include
the earlier root `research/` package or the divergent FOE research documents.
This follow-up preserves 26 original text blobs without editing their bytes.
Their Git blob SHA-1 values were verified against GFG Extreme before import.
Paths, source commits and external evidence are recorded in
[GFG_RESEARCH_MANIFEST.json](GFG_RESEARCH_MANIFEST.json).

## Native FG / MAKO / LSFG, 2026-10-15

Source: [GFG Extreme `609ce4f`](https://github.com/wavessevaw/GFG-Extreme/tree/609ce4f50c95c80135348b6c0e3dc7270d8fda2f/research).

- [Original research index and gate status](native-fg-20261015/research/README.md).
- [MAKO frame lifecycle](native-fg-20261015/research/MAKO_FRAME_LIFECYCLE.md),
  [synchronization](native-fg-20261015/research/MAKO_SYNC_ANALYSIS.md) and
  [pacing](native-fg-20261015/research/MAKO_FRAME_PACING.md).
- [Historical GFG engine audit](native-fg-20261015/research/GFG_CURRENT_ENGINE_AUDIT.md).
- [Host-call recorder experiment](native-fg-20261015/research/MAKO_HOST_CALL_EXPERIMENT.md)
  and [JSON summary](native-fg-20261015/research/results/mako_host_trace.json).
- [LSFG binary-analysis limits](native-fg-20261015/research/LSFG_BINARY_ANALYSIS.md)
  and [pinned sources](native-fg-20261015/research/SOURCES.md).

The 36 compressed recorder traces remain in GFG Extreme, with pinned URLs and
blob hashes in the manifest. These traces are **simulated host-call evidence**,
not GPU images or Deck timings. Original scripts and cross-directory links
resolve in the [pinned source tree](https://github.com/wavessevaw/GFG-Extreme/tree/609ce4f50c95c80135348b6c0e3dc7270d8fda2f):
[probe](https://github.com/wavessevaw/GFG-Extreme/blob/609ce4f50c95c80135348b6c0e3dc7270d8fda2f/tools/native_fg_probe.py),
[host recorder](https://github.com/wavessevaw/GFG-Extreme/blob/609ce4f50c95c80135348b6c0e3dc7270d8fda2f/tools/trace_mako_host.py),
[provenance](https://github.com/wavessevaw/GFG-Extreme/blob/609ce4f50c95c80135348b6c0e3dc7270d8fda2f/docs/PROVENANCE_AND_LICENSES.md).
Those tools and dependencies are not installed by this documentation import.

## Alternative Frame Output Engine, 2026-10-17

Source: [GFG Extreme PR #152](https://github.com/wavessevaw/GFG-Extreme/pull/152),
pinned at `f7bbcddf11419db4005d87b23b3d992e35795819`.

- [Ownership architecture](foe-20261017/docs/gsf-output-engine/ARCHITECTURE.md).
- [Prior-art review and correction](foe-20261017/docs/gsf-output-engine/PRIOR_ART_REVIEW.md).

Its standalone colored-image probe and numeric queue are historical prototypes;
their source remains in the original branch. The targets named in these
documents are not added to GSF by this import. Current GSF has its own
Output Owner contracts and real Vulkan tests. A proxy swapchain was a candidate,
not a proven requirement or a claim about the Windows Lossless Scaling binary.

## Consequences for current development

The old evidence distinguishes backend work, source readiness, GPU completion,
WSI handoff and physical display. A host graph or successful present does not
prove visible 2×, and a delayed deadline does not reduce already submitted GPU
work. Reusing pair analysis is already an LMC property; it is not a new fix.

GFG PR #149's two-refresh-tick buffer changes `gfg_frame_clock_pair` and delays
both real and synthetic output. It is intentionally not imported into runtime.
Current GSF keeps the 0.1.6 clock/LMC baseline while calibrated measurements
are checked. Admission must follow actual deadline/slack and measured cost,
without imposing a new minimum source-FPS rule.

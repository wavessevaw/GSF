# LMC first dispatch: controlled disk-cache experiment and unused pipelines

## Reproduced baseline, not a Deck performance claim

[Run 39112249673](https://github.com/wavessevaw/GSF/actions/runs/39112249673),
commit `c3dfc0198c2847a27fd9c5e1bec2b61bf0f0a8cd`, direct Temporal GPU LMC,
96×64 deterministic integer translation, four source pairs, two outputs per
pair, tier 0, Vulkan validation. Each cache condition starts a fresh process.
An invocation-specific temporary cache directory isolates pre-existing caches.

| Software GPU runner | Empty cache first pair GPU | Reused cache first pair GPU | Disabled cache first pair GPU | Later pairs, empty-run median |
|---|---:|---:|---:|---:|
| Ubuntu 22.04 | 21779.33 ms | 87.01 ms | 21743.33 ms | 64.16 ms |
| Ubuntu 24.04 | 17623.07 ms | 44.51 ms | 17632.73 ms | 15.07 ms |

The first empty-cache GPU-model stage costs 10.18 s (22.04) / 6.24 s (24.04).
The two motion searches cost another 9.11 s / 8.23 s combined. These are
command-stream timestamp intervals; they do not individually prove whether
LLVM compilation, execution, or driver synchronization occupies each interval.
The controlled cache intervention demonstrates a large cache-sensitive
first-execution cost. Do not characterize the full ~22 s as shader-module or
pipeline creation: context creation was ~1.56 s / ~1.53 s, separately measured.

All six cases passed four-pair/two-output accounting, no Vulkan validation
errors, positive GPU timing records and distinct intermediates (no endpoint
holds). All generated pixel arrays matched exactly across both runners and
all cache states, SHA256:
`ba6de83ed0a533c2ca047939016ce24ea17414ce23f8a9d3bc0cbbd40ce659c0`.
That is determinism on this fixture, not a broad interpolation quality score.

## Concrete initialization fix

`te_context_create()` previously built all nine ordinary tile pipelines and
then built a separate LMC pipeline family. The algorithm is fixed in the
context. Every analysis and output submission chooses `lmc_record_*` whenever
the context has LMC, including holds, history, CPU-fit LMC and shared flow.
The ordinary tile pipelines are never bound in those contexts.

Create the tile pipelines only in the tile algorithm branch. Preserve
algorithm-specific layouts, buffers, synchronization, LMC pipelines and
error cleanup. The profiler's `temporal-*` pipeline events must now be absent
in the direct GPU LMC lab; its real pixel/timestamp/validation checks still
apply. Full existing Temporal tests exercise tile, CPU LMC, GPU LMC and shared
flow; native GPU WSI correctness continues in both layer orders.

This removes nine redundant Vulkan pipeline-creation calls per LMC context.
It is not presented as a solution to the much larger first-dispatch
cache-sensitive cost, and no constant startup speedup is promised across
drivers. Compare the pre-change run above with the subsequent PR #18 runs
for observations; separate runners introduce timing variability.

## Reproduction and limits

```sh
make -C engine/gfg-temporal all lib
export VK_ICD_FILENAMES=/path/to/lvp_icd.json
python3 -m unittest discover -s engine/gfg-temporal/bench -p test_startup_cache_lab.py -v
python3 engine/gfg-temporal/bench/startup_cache_lab.py --out startup-results
```

The lab saves generated/source NPYs, JSONL stage timings, initialization
profiles, full process logs and a result JSON. Temporary disk-cache contents
are not retained; only file counts are observed. A directory reuse is not
claimed to be proof of a particular driver's cache hit. Mesa's documented
controls are used:
https://docs.mesa3d.org/envvars.html#envvar-MESA_SHADER_CACHE_DISABLE
https://docs.mesa3d.org/envvars.html#envvar-MESA_SHADER_CACHE_DIR

Host profiling reuses PR #6's implementation (9e482f3, f203d16), without
merging its separate strict-unpaced CI. No assertion in PR #4 is weakened.
This lab has no native-layer WSI or physical display output. It does not
establish RADV performance, 45→90 deadlines, game quality, or OLED scanout.
A production prewarm/cache policy remains a separate measured design task;
no global Mesa cache setting, game launch option or SteamOS file is changed.

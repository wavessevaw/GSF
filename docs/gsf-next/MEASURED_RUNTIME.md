# Measured runtime selection

The objective is lower generation cost while preserving source rendering and
output cadence. A particular algorithm name, copy count, or topology is not a
performance result. Alpha 5 did not demonstrate a uniform direct-output speedup;
this iteration replaces that fixed default with GPU-timed selection.

## Actual device measurements select the route

Normal operation can reconstruct through shared scratch plus copy, independently
into private outputs, or with shared-pixel multi-output reconstruction. All routes
use the same source pair, motion field, temporal coefficients and pixel equations.
Only one route runs for a pair. Trials generate only outputs admitted for display;
no extra benchmark images, duplicate analysis, full-frame CPU readbacks, or
synchronous query waits are introduced.

The application retains the existing bounded WSI/output lifecycle. One scratch
image is deliberately allocated again in automatic mode: its roughly 4-MB cost
at 1280×800 RGBA8 is preferable to forcing a slower path to preserve an architectural
claim. Fixed direct/batch overrides can still omit this allocation. The private
output ring stays bounded and is not enlarged.

Separate selectors for one/two/three outputs avoid comparing incomparable costs.
After the first twelve source frames and excluding debug readbacks, each available
route receives eight GPU observations, interleaved across real output pairs. Each balanced round rotates the trial order
to avoid aliasing a route with a work slot or frame parity.
The score is 75% median and 25% upper order statistic, rejecting a single scheduling
outlier. Changing the incumbent requires more than 10% plus 0.02 ms improvement.
This is a conservative choice among measured routes, not a claim to find a global
optimum or to improve every individual frame.

Measurements include reconstruction, scratch copies when used, and final layout
dependencies. Shared capture/motion analysis is measured separately and excluded
from route ranking. Per-slot route/output-count/epoch tags associate retired GPU
queries with the job that produced them. Late results from a previous calibration
cannot influence a new one.

After convergence, there are no periodic benchmark bursts. At least 128 winner
observations plus eight consecutive costs above 150% of baseline + 0.05 ms are
required before re-evaluating alternatives. An isolated hitch does not restart
selection. Without reliable timestamps, exploration is bounded to 128 choices;
repeated query-read failures or unavailable timers retain the conservative copy
route. Multiplier, flow resolution, image quality thresholds and display-slot
admission do not change as part of route selection.

## Three GPU markers during ordinary play

Detailed diagnostic timing is no longer the ordinary cost. With diagnostics off,
three timestamps cover start, completed shared analysis, and completed output.
Optional per-dispatch markers, presenter copy timestamps and per-pair diagnostic
files are disabled. The presenter query pool stays resident so diagnostics can
change live without allocation or a source-thread wait.
This removes dozens of potential timestamp/stage synchronization events per pair.
A total reconstruction interval remains measurable; per-output G1/G2/G3 durations
are not invented from it.

Diagnostics enables detailed stage timing and preserves route/reconstruction
provenance. Cached command buffers include timing detail in their cache key, so
changing diagnostics live cannot replay stale query indices. Calibration also
resets on a timing-layout change; epochs reject old-layout observations. Timestamp deltas mask
the queue's valid bit count. No new GPU fence waits are added to source rendering.
`renderer` and `reconstruction_ms` in native snapshots identify the last measured
route and cost, independently from the selected multiplier or physical FPS.

## Overrides and verification

`GSF_NEXT_RENDERER=auto|copy|direct|batch` selects an explicit process-start route
policy. `auto` is the default. Legacy copy/batch diagnostic overrides remain
supported. `GSF_NEXT_PROFILE_DETAIL=0` retains the three-marker layout while
collecting diagnostic JSON for controlled experiments. Place overrides after the
managed launcher's `--` using `/usr/bin/env`, as in ALPHA5_EFFICIENCY.md. Ordinary
multiplier/quality/diagnostics settings remain live.

Deterministic policy tests cover each possible winner, noise, isolated/sustained
hitches, per-output-count separation, stale observations, unavailable methods and
invalid/missing timings. Actual Vulkan tests exercise all three routes on the
same live resource set, verify unchanged pixel/flow/HUD thresholds, run synchronization
validation, and require exactly three timestamps in compact mode. The rest of the
Vulkan, source lifecycle, hot-setting, Decky and extracted-package checks remain.

Software Vulkan measurements are evidence of implementation and behavior. They
are not Steam Deck performance, physical FPS, wattage, or input-latency results.
The selector uses measurements from the user's own GPU; this does not establish
hardware qualification without a Deck run. See MEASURED_RUNTIME_REPORT.md and the
hardware acceptance protocol. Stable/main is not replaced by this experiment.

# Deck traces, Auto mode (2026-10-21)

Two KMS flip traces recorded by the user on a Steam Deck OLED (90 Hz) with **Pace → Record
pacing trace**, Auto mode, sent on 2026-10-21. Analysed with `kedavra/tools/kfg_pacing.py`
(reports next to each trace). The plugin build on the Deck is not recorded in the trace; it was
the one installed at the time (1.1.0 for the 10:53 trace, an earlier build for the 04:37 one,
to be confirmed by the user).

| Trace | Length | P95 interval error | P99 | Missed refreshes | Irregular content steps | Result |
|---|---|---|---|---|---|---|
| `20261021-043758-auto-x3` | 413 s | 11.11 ms | 22.22 ms | 0.51% | 14.1% | FAIL |
| `20261021-105329-auto-x3` | 840 s | 0.0016 ms | 0.0026 ms | 0.27% | 0.64% | P95 / P99 / missed **meet the targets**; content steps 0.64% (limit 0.5%) |

The 10:53 trace is the first hardware measurement in this project that meets the pacing
targets (P95 ≤ 1 ms, P99 ≤ 2 ms, < 0.5% missed) over a long run (14 minutes, 75 214 refreshes,
almost all at x3, 693 refreshes at x2). It does not pass the analyser's uniform-motion check:
0.64% of the new frames advance the content by more than one step, mostly two (434 of 75 000),
from 138 repeats because a pair's analysis was not ready in time (`me_late`), 51 because the
game was late (`source_late`), and 477 skipped generated frames. This is one trace of one game
session; image quality is not measured by it.

The 04:37 trace was cut short (its last line is a run of NUL bytes); the analyser now drops such
rows instead of failing.

How to read the interval error: `flip_ns` are the kernel's KMS page-flip (vblank) timestamps, so
an interval between two newly presented frames is a whole number of refresh periods plus the
timestamps' rounding. In the 10:53 trace, 74 634 intervals lie between 11.108 and 11.112 ms (the
period is 11.1106 ms) and 112 are 22.22 ms (a missed refresh). The P95 / P99 of a few
microseconds is that rounding: the targets mean here that at least 99% of the new frames
arrived exactly one refresh after the previous one. In the 04:37 trace more than 5% did not.

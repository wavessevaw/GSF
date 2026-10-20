# Runtime efficiency: final-binary software evidence

**CPU Vulkan / llvmpipe LLVM 15, two workers, a two-CPU quota. Not Steam Deck/RDNA2. No physical FPS, power or input-latency qualification.**

One final-binary serial Alpha 5/automatic comparison at 640×480, 80 source frames, ×4; first sixteen frames excluded. Baseline is the published Alpha 5 binary. New ordinary timing uses three generator timestamps and no optional presenter timestamps. The old generator used per-stage timestamps and timed output copies. New pair timing also includes final layout dependencies, unlike the old pair interval.

This is an overall implementation comparison, not an isolated optimization measurement. The 360-Hz software admission budget requests three intermediates; it is not a display rate. There are no validation or pixel readbacks during timing. Cost percentiles include only fully admitted three-output pairs, with coverage retained. Conditional costs do not prove sustained ×4 output or source FPS preservation.

| Output | Runtime | Pair p50 / p95 / p99, ms | Steady pair p50, ms | Complete / warm pairs |
| --- | --- | --- | ---: | ---: |
| 640x480 | baseline | 47.423 / 93.852 / 115.710 | 47.305 | 64 / 64 |
| 640x480 | auto | 41.547 / 67.401 / 70.581 | 36.600 | 64 / 64 |

**Both 1280×800 runs FAILED the unchanged minimum-sample requirement.**

| Runtime | Complete / warm pairs | Required complete pairs |
| --- | ---: | ---: |
| 1280x800 baseline Alpha 5 | 36 / 64 | 40 |
| 1280x800 automatic | 27 / 64 | 40 |

The automatic run recorded a maximum pair interval of 3309.747 ms and reconstruction interval of 3250.529 ms on the software driver, far beyond the 250-ms source-gap guard. Admission gaps and too few complete pairs prevent a credible performance comparison or convergence qualification at this resolution. Its results are retained as failures, not included as passing performance percentiles. No sample threshold or source-gap guard was relaxed.

Earlier fixed-order development measurements were excluded from conclusions because their route order could alias job slots and history parities. The final selector uses balanced rotating rounds.

The accepted 640×480 automatic run compares costs inside its own context:

| Initial copy / direct / batch score, ms | Steady actual route counts | Settled observations |
| --- | --- | ---: |
| 39.518 / 37.097 / 27.539 | {'batch': 32} | 32 |

These limited software observations do not establish a universal speedup, stable tail latency, energy savings, or hardware superiority. The selector can retain an incumbent within its uncertainty margin, and overload or nonstationary timings can prevent useful selection. Missing slots and excluded pairs have not been relabeled as successful generation.

The separate 30-case Vulkan pixel/ownership matrix, live setting/diagnostic transitions, policy contracts, Decky checks and extracted launcher test implementation behavior. Compact timing explicitly checks three generator timestamps and disabled optional presenter timestamps. The packaged native payload must match the final measured candidate SHA-256.

The efficiency goal remains unqualified on Steam Deck: native game FPS, physical cadence, satisfactory quality, power and input latency need a hardware run. See MEASURED_RUNTIME.md and DECK_PROTOCOL.ru.md. Stable/main is unchanged.

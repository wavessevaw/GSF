# Retained trace reconstruction (G0)

| Retained measurement | x3 | x2 |
|---|---:|---:|
| Pairs / source span | 7673 /169.599s | 3849 /85.485s |
| Real WSI accepted | 7673 | 3849 |
| Synthetic WSI accepted | 1 | 66 |
| Actual dispatched pairs/outputs | 84/84 | 168/168 |
| Matched calibrated starts beyond deadline, using reported maxDeviation | 81/84 | 42/168 |
| Submit-return to GPU-start median | 41.682ms | 7.513ms |

Exact per10-second windows, archive/build identity and invalid tail-line counts
are in `evidence/trace_reconstruction.json`. Joins use pid/generation/actual
te_submit_sequence, excluding bypass rows. The prefixes are missing; summary
counters for the full sessions cannot restore their timing or phase history.
Exit -2 is not by itself crash evidence. The same script checks the two raw
sessions, not the handoff author's derived CSV alone.

Three distinct facts: GPU compute produces a temporal image; WSI accepts its
semaphore-backed present; compositor reports timing. None alone establishes
unique physical OLED output. CPU source entry is not GPU source readiness.
The start delay is dependency/delivery plus queue/driver behavior, not a measured
pure hardware queue wait. No source-only same-scene Deck baseline is available.

x3: 4433 adjacent mask2 cases are exact planned midpoints. The reconstruction
finds4627 valid adjacent mask2 rows, one more than the handoff's4626 boundary
count; it finds the same4433 exact midpoints. Of those4627 rows,4591 differ from
the old nominal2/3 phase (other lattice spans include3/5). This is a deterministic
content/time mapping defect independent of the GPU-start lateness. Its existence
does not prove that it alone caused the reported frametime variation.

A separate code defect remains in this base: te_poll/gpu_sample is guarded by
`fg->context` inside create immediately after calloc, so that call is a no-op.
The patch performs the nonblocking poll at every present entry, including
admission bypass, to consume newly completed samples without needing a new
TE submission. The per-pair pace_real reset introduced by0.1.10 is retained.

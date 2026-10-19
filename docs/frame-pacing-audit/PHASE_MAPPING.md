# Interpolation phase contract

For the presentation-plan model, real A/B endpoints are the consecutive planned
real targets. For a reserved slot S, tau=(S-A)/(B-A), strictly0<tau<1. Nominal
multiplier labels are not phases. x3 mask2 at45→90 has one slot at tau1/2, emitted
from output_index0/destination0 although its logical slot_id is1 (G2).

`frame_clock.previous_real_ns` snapshots A before advancing the baseline grid.
The phase helper rejects missing, discontinuous or noncausal anchors. Pacing-off
legacy compute keeps its requested nominal fractions. No fallback nominal phase
is substituted into an invalid paced slot. Float rounding is checked strictly.
A source jitter remains source jitter; this model is not an assertion that
planned A/B times were physically displayed at those times.

`prepare_generation` uses the actual admitted mask to map compact TE outputs to
logical slots and their phases. Signal mapping remains v+3-count+j. G2 without
G1 is therefore one output with its own signal; it does not borrow destination1.
`slot_mapping` logs pid/generation/sequence, both source IDs/timestamps, planned
A/B, slot ID/target, requested/applied tau, output/destination index and wait/signal.
Deferred SharedFlow G2 is explicitly marked recorded_deferred, not submitted.

`frame_phase.c` calls the actual production prepare_generation across x2/x3,
source30/45/60/75/90, output60/90 and jitter0/0.2/1/3ms, and replays all10273 retained
phase rows. It reproduces4433 exact mask2 midpoints. A successful phase unit does
not qualify frame pacing or software/Gamescope/Deck display output.

The old opt-in adaptive_pair mechanism can retarget G1/B after GPU readiness.
That remains a separate architectural issue: a new owner must freeze a pair's
phase anchors and deadlines before dispatch, or cancel that synthetic output.
It must not reinterpret a completed image as a different temporal phase.

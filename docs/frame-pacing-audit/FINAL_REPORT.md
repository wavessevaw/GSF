# 0.1.12 audit and candidate status

Work is on `experiment/0.1.12-frame-pacing`, draft PR45, based on checked PR44
head ecda1fb382eefe0f8adff65911687c4ec5750b99. Main is unchanged. The handoff
and raw input hashes are in SOURCE_INVENTORY.md; reproduction emits the phase
fixture and causality JSON. Exact source SHA and CI artifacts are attached to
PR45. BUILD_INFO.json in the packaged ZIP identifies its actual source commit;
archive hashes are recorded after CI packaging, outside a self-referential commit.

The confirmed faults corrected here are wrong interpolation phase on x3 sparse
slots, post-compute adaptive retargeting, deadline override in visibility retry,
misplaced completion polling, destructive frames-log truncation and the missing
installed exporter module. Repeated adaptation now requires observed GPU-ready
and WSI-accepted work, and distinct recovery failure stages appear in diagnostics.
There is no source-FPS minimum and no main-thread per-frame GPU wait introduced.

A deterministic immutable-ticket reference passes sufficient-headroom45→90x2
and30→90x3 over600 virtual seconds plus640 changing-load scenarios. A separate
installed Vulkan-layer proof generates actual LMC pixels and sends them through
one WSI, including x3 G2-only τ0.5. Duplication is a negative control. These are
software proofs with explicitly narrower scope than hardware gameplay.

The deeper blocker remains: a41ms start delay plus compute cannot fit an8ms
unbuffered handoff window. The live serial source-priority WSI path cannot promise
all intermediate frames under that condition. A44.444ms extra planned buffer is
feasible for one modeled budget, but its production resource owner and physical
latency contract are not yet qualified. It is deliberately not exposed as a
working game mode. See PACING_ARCHITECTURE.md and BUFFERING_LATENCY_BUDGET.md.

G0: PASS. G1: model PASS, wall-stress results tracked on PR. G2: partial software
transport/phase proof; complete real-time buffered-owner acceptance remains OPEN.
G3: NOT TESTED;15W Deck acceptance NOT MET. A successful CI run must not change
these hardware statuses. This is a proven limitation of the current schedule
with a verified alternative GPU transport direction, not a claim that sustained
Deck generation is finished.

Rollback: disable GSF for the selected game and restore its saved launch string
using the plugin; installed launcher passes through when disabled or payload is
missing. Prior source/archives remain available. Do not merge this experiment
into main or replace a stable installation automatically.

The local long run also exposed a post-readiness handoff tail: max69.800ms after
the planned G target. This is an additional proven limitation of the direct WSI
path. The prototype is not shipped as a remedy for that tail. Candidate packaging
remains a correctness/audit artifact; a new manual Deck test is not required to
establish this already reproduced software blocker.

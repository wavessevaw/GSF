# GSF 0.1.12 source and evidence inventory

Base / checked PR44 HEAD: `ecda1fb382eefe0f8adff65911687c4ec5750b99`.
Branch: `experiment/0.1.12-frame-pacing`. Main is not modified.

User handoff ZIP SHA256: `fc84866bbd4b6b6c4db260263af9015aa8292fc8bc13711a97401db19a3ddcf0`.
The embedded `evidence/game-582160-20261019T08.tar.gz` is actually present and read.
Its SHA256 is `4bc2a90661210dd83fb8300c6ea791ca64b6e68bee736d2584bb513f5af133ae`.
Both LAUNCH files identify build `6a5d66ec6384be371d42742ee054bda1617d5a7c`.
That is the tested PR merge build, not the PR head. The base does not incorporate
new unverified hardware evidence merely by changing its version number.

Two retained sessions: app582160, x3 source IDs11512–19184, x2 IDs12095–15943.
Their JSONL was truncated by the old launcher. Earlier 30FPS settings cannot be
reconstructed from the retained tails. TDP15W is user-reported; neither archive
verifies the applied limiter. Global AMDGPU power/busy is not game attribution.

Reviewed paths: frame_clock, prepare_generation, present_synthetic, output_present,
output_worker, adaptive_pair, generation_budget, calibrated_clock, timeline_watch,
present_chain; TE submit/output timestamps/deferred outputs; Decky launcher,
runtime export and offline diagnostics. Existing 0.1.8–0.1.11 mechanisms remain
experimental. In particular the opt-in adaptive pair path is x2 only; baseline
x3 still uses the ordinary deadline path. Shadow does not own scheduling.

The 0.1.5–0.1.7 calibrated evidence is separately preserved in the earlier audit
PR40. The 0.1.8 adaptive pair experiment added bounded B extension; 0.1.9 added
shadow observation; 0.1.10 reset real pacing per pair and sampled shadow on early
exits; 0.1.11 added persistent x2/x3 selection. None proves continuous OLED90.

Reproduce retained reconstruction:

```sh
python3 tools/reconstruct_pacing.py /path/to/game-582160-20261019T08.tar.gz \
  --out docs/frame-pacing-audit/evidence
make -C engine/gfg-temporal lib
make -C engine/gfg-native frame-phase
python3 -m unittest discover -s decky/tests -p 'test_*.py'
```

No full user game/account paths or proprietary assets are committed. The phase
fixture contains numeric event identities and timestamps, not raw game files.

# Acceptance matrix

| Gate | Evidence | Scope / limit |
|---|---|---|
| G0 | Both retained raw traces reconstructed; all10273 phase rows replayed through production prepare_generation;4433 exact mask2 midpoints | PASS for retained interval; initial30FPS history was truncated |
| G1 headroom | 600 virtual seconds each:45→90x2 all27000 optional slots;30→90x3 all36000;60→90 x2/x3 capacity-limited;90→90 x2/x3 zero synthetic | CPU event model, no physical FPS claim |
| G1 robustness |640 scenarios,600 simulated seconds each; source30/45/60/75/90, output60/90, x2/x3, jitter±0/.2/1/3ms, delays.1/7/15/35/50ms, costs5/7/10/15ms, buffer0/4refresh | Bounds four modeled synthetic resources; not complete real source+WSI pool model |
| Recovery | Actual x2 policy2200 variable steps; repeated congestion/probe/ready+WSI recovery, immutable targets; accepted without readiness does not recover | Deterministic policy test |
| Phase/transport G2 | Real installed layer, real LMC, GPU readback all6144pixels/output, x2/x3/G2-only and duplicate negative control | Uniform grayscale proves interpolation/transport, not moving-game quality or panel output |
| Normal long stress |600 wall seconds x2 and x3 in CI, changing source5/30/45/60/90, jitter, pause, recurring60ms INPUT-ready delay, segmented logs, validation | Input readiness injection is not Deck hardware queue emulation |
| Packaging | Frontend/type/panel/backend tests; exact archive extracted, installed, x2 and x3 real Vulkan launches; exported hashes, disable/uninstall bypass | Steam UI and Deck are not present in CI |
| G3 | No authenticated Deck endpoint or /dev/dri in executor | NOT TESTED / hardware acceptance NOT MET |

Commands: `make -C engine/gfg-native frame-phase pacing-owner-model adaptive-pair`
(after TE build with pinned Vulkan headers); `python3 -m unittest discover -s
decky/tests -p 'test_*.py'`. Workflow `gsf-frame-pacing.yml` records real Vulkan
owner logs and two ten-minute normal stress archives. `tools/check_pacing_lab.py`
checks identities, phases, output order, retained JSONL and end-of-run state.
All existing CI workflows are required too. See PR45 for checks on the exact SHA.

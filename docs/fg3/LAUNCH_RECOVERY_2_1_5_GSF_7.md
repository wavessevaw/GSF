# Launch recovery on current 2.1.5, experimental GSF ×2/×3

The owner reported every game exits immediately, including with the suggested
`/home/deck/.local/bin/gfg-native --off %command%` option. That does not establish
a GPU crash: a missing/unexecutable launcher never reaches the game or Vulkan.
The exact on-device cause is **unconfirmed**. Removing the entire GFG prefix is
the baseline recovery; another manual log series was not requested.

## Delivered changes

The integrated package now uses main 25ca625 (2.1.5, including PR #140/#141 fixes),
with independent native code from PR #133. internal.6 had the stale 2.1.2-te.8 base
and is withdrawn. Package/lock/backend/report version identities agree on
2.1.5-gsf.7; the separate native component identifies internal.7. The complete
corresponding source is 9ddb5802e3cac33df0f5c50df6b67ff55d2d892a.

| Reproduced defect | Implemented behavior |
|---|---|
| Native installation existed only behind the mode click; absent bundle reported success | Plugin startup verifies/stages the bundled component and executable; missing installation rejects mode activation and shows an error |
| Status checked the ELF/manifest but ignored the Steam executable | Readiness requires an executable launcher; UI shows the command only for a ready component |
| No marker meant Off in UI but ×3 in the standalone CLI | Integrated startup initializes an Off marker and preserves an existing explicit choice |
| `--off` retained inherited native Vulkan layer activation | Bypass removes only GFG's exact names from layer activation lists, preserves foreign layers/arguments/exit status and needs no optional settings helpers |
| Failed status/diagnostic directory creation stopped the game | Status is optional; unavailable diagnostic capture runs the original game and preserves its status |
| Initial rollback/uninstall left an option-blind `exec "$@"` stub | The retained wrapper understands `--off` and bypasses cleanly without the component |
| Repeated staging overwrote the useful rollback snapshot | Identical verified payloads are idempotent; the previous distinct installation remains restorable |
| Failed patched CreateDevice consumed the downstream LINK_INFO cursor before original-request retry | Restore the loader-owned cursor before real-only retry; application feature/extension arrays remain untouched |
| Package cleanliness omitted included build-helper/docs changes | Every included corresponding-source path participates in the clean-commit check; both archives have checksums |

These are fixed code defects, not a claim that one has been identified as the
owner's crash cause. Native GSF retains separate ×2 midpoint and ×3 thirds, GPU
source/history, bounded asynchronous presentation, source-time deadlines and
real-only fallback. No replacement algorithm, power change or hidden foreign FG
fallback was introduced.

## What testing establishes

- Current merged backend: 1409 tests, zero failures, 1 root-only privilege-drop skip.
- Browser frontend:123 screens. Native queue/clock/metadata/device-retry contracts pass.
- GSF pipeline: 17 checks, 183 core assertions, actual G1/G2 pixel/order/spacing
  observation for ×2/×3, duplicate/cut/lifecycle/unknown-metadata scenarios, and
  both validation-layer orders. This uses a 9 Hz software clock.
- Exact extracted archive: 13 checks. The backend is imported from the ZIP;
  actual plugin startup stages the launcher even when legacy renderer setup is
  unavailable. Initial Off, ×2, ×3, read-only Vulkan12 features, Proton present
  metadata, inherited activation, absent component, failed optional capture and
  rollback run through actual lavapipe/X11 WSI with zero validation errors.
- The installed-production-ELF CI scenario was also run locally with explicit
  GPU verification: G1/G2 are distinct and ordered in that fixture. This is
  test-only observation; normal runtime has no pixel readback.
- Production ELF requires GLIBC2.17 symbols, loads with the actual glibc2.31
  loader, and rebuilds byte-identically from the final archive's included source.
- Five new recovery checks were also run against internal.6: 3 assertion failures
  and 2 execution errors, intentionally preserved as negative controls. They are
  not passing acceptance results.
- CI source now checks GSF rather than the obsolete `algo=lmc-gpu` expectation,
  uses explicit software source/output clocks, builds both archives and preserves
  evidence. Successful remote CI is **not claimed**.

Hashes and raw logs: evidence/launch-recovery-2_1_5-gsf_7.json and its matching
ZIP. The source/build instructions saved for the environment now select the
integration checkout instead of the old stable 2.1.1 checkout; the refresh script
was executed successfully. Saving that draft does not publish a new environment.

## Barrier that remains

No `/dev/dri` or authenticated Steam Deck endpoint is available in this cloud;
device-inventory ZIPs contain metadata only. SteamOS/Gamescope transport, real
game stability and contention, physical 90 distinct OLED frames, power and latency
remain **NOT TESTED**. Software WSI acceptance is not panel scanout. The prior
negative 90 Hz software result remains negative.

Known generator barriers are preserved: the single application queue still puts
G1's transport copy behind G2 compute; the renderer cut case was missed; HDR PQ
interpolation is not linear-light. No promise that these are solved by launcher
repair is made. The product goal 30 real→90 actually displayed remains **OPEN**;
this is an installable experimental update, not product completion or a stable
release. Hardware acceptance remains the concrete next stage in
HARDWARE_ACCEPTANCE_1_1.md, without sending the owner another manual-log request.

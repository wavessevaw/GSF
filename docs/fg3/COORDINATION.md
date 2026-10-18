<!-- codex-fg3-coordination -->

## User-authorized prerelease — final internal.10 candidate

Owner reports internal.9 launch works and requests a checked GitHub prerelease.
This is a launch report, not measured physical90. Final source **db04b3dfb5dbe3e2dac66761de7e397b1315b0fc**.
The first unpublished .10 candidate failed remote WRITE_AFTER_WRITE validation
(run38992642512): compute writes to temporal predictor were not available to
its transfer overwrite. Destination TRANSFER_WRITE is now included in the
existing barrier. This is a synchronization correction; algorithm/shaders stay
unchanged. Decky Off also now wins over conflicting inherited XDG_CONFIG_HOME,
while explicit MODE/MODE_FILE and standalone XDG settings remain supported.

Final artifacts are GFG-Extreme-2.1.5-gsf.10.zip and native internal_10.zip,
with SHA256SUMS.txt. The previous preparation's .10 ZIPs were never tagged or
released; their exact blobs remain in Git history. Final production ELF hash:
4ab805fc97eaff7b04869e3ec8be92a31e54b9a504557b8d89b48d128ff64c62.

Local1410 backend (1skip),48 native,123 UI,17 pipeline,7HDR/legacy and14 exact
ZIP checks pass. Production loads with actual glibc2.31 and rebuilds byte-identically
from its included source. Old9 fails the XDG Off negative control. Final evidence:
evidence/prerelease-internal_10.json and ZIP. Remote repeat checks are required
before tag v2.1.5-gsf.10 can publish. Branch verification alone does not publish.
No stable/latest change or main merge. Remaining cut, real-game GPU cost,
physical90, HDR quality and latency/power acceptance stay open.

## G1-before-G2 implemented — internal.9 experiment

Source **7e8a9660a0c40e00275f87f0977c0758e6f3ebe3**; fast-forward published to both PR133 head and integration.
Artifacts: `docs/fg3/artifacts/GFG-Extreme-2.1.5-gsf.9.zip` and
`GFG-Native-FG-v0_0_0-fg3-internal_9.zip`; SHA256SUMS.txt beside them.
Evidence: `docs/fg3/evidence/deferred-g2-internal_9.json` and ZIP.

The bridge now submits G2 only after G1 copy and lower QueuePresent return,
removing its previous self-imposed G1 dependency on G2 compute. It cancels
unsubmitted G2 on detected cut/duplicate/expired deadline and retires the final
signal/fence without reconstruction. Failed G2 submissions wait only the known
G1 timeline during cleanup, never an unsubmitted final fence. x2 is unchanged.
No separate GPU queue, physical timing or measured Deck speedup is claimed.

Borrowed-device test proves G1 pixels are readable while G2 is unsubmitted,
then checks resume, cancel accounting/control, poll/idle/destroy and injected
G2 submission failure. 17 pipeline checks pass in both layer orders, including
G1-before-G2 telemetry and cut/duplicate cancellation. 7 HDR/legacy checks,
47 native Python tests and native contracts pass. 14 checks against the actual
extracted ZIP pass, including installed production ELF x3 ordering and x2.
Actual glibc2.31 load and byte-identical included-source rebuild pass.
The previous full backend/UI results are retained as prior checkpoint evidence,
not rerun or misreported as hardware acceptance for this C change.

Remaining barriers: missed renderer cut; shared app queue game contention and
HDR cost/quality; no authenticated Deck or /dev/dri for Gamescope/DRM/OLED
physical30-to-90, latency/power or all-games-fixed acceptance. Owner crash cause
remains unconfirmed on-device. Product task stays OPEN. No stable release/main
push or additional architecture/PSNR research was undertaken.


## HDR PQ correction implemented — internal.8 experiment

Source **43658e884d67326183d3fce749840053a0c1cebe**.
Artifacts: `docs/fg3/artifacts/GFG-Extreme-2.1.5-gsf.8.zip` and
`GFG-Native-FG-v0_0_0-fg3-internal_8.zip`; checksums beside them.
Evidence: `docs/fg3/evidence/hdr-pq-internal_8.json` and ZIP.

Preserve actual swapchain color space; supported HDR10 ST2084 reconstructs in
GPU-cached linear nits, shared by x2/x3. Unknown transfers give explicit real-only
fallback. Hold/cut/HUD endpoints and duplicates retain original codes. No tone map
or power change. The31 previous shaders are byte-identical; PQ alone allocates
15.625MiB at1280x800 and uses separate shader/layout variants.

Six numerical/precision scenarios pass for both host and glibc-SDK core, including
x2/x3 and1024 levels; the old coded-space implementation fails the same luminance
oracle. The first duplicate-roundtrip regression and its fix are preserved.
Format58 BLIT_DST is unsupported here and skipped explicitly; format64 is tested.
17 GSF pipeline checks,47 native Python tests, native contracts and13 exact-archive
WSI/startup/recovery checks pass. Production ELF loads under actual2.31 and
rebuilds byte-identically. No HDR WSI, Deck cost, game quality or physical90 claim.
G1 transport ordering, the missed renderer cut and hardware acceptance stay open.


## Launch recovery: published 2.1.5-gsf.7 experiment for PR #133

Source: **9ddb5802e3cac33df0f5c50df6b67ff55d2d892a**.
Upstream main **25ca625** (2.1.5, including PR #140/#141) is merged.

- Integrated Decky: `docs/fg3/artifacts/GFG-Extreme-2.1.5-gsf.7.zip`
- Component: `docs/fg3/artifacts/GFG-Native-FG-v0_0_0-fg3-internal_7.zip`
- Checksums: `docs/fg3/artifacts/SHA256SUMS.txt`
- Evidence: `docs/fg3/evidence/launch-recovery-2_1_5-gsf_7.json` and matching ZIP
- Review: `docs/fg3/LAUNCH_RECOVERY_2_1_5_GSF_7.md`

The owner reported all-game exits even with `gfg-native --off`. Exact Deck cause
remains UNCONFIRMED; removing the whole Steam prefix is the recovery when its
executable is absent. No further manual log series was requested. internal.6
used stale 2.1.2-te.8 and is WITHDRAWN below.

Fixed: automatic verified component/launcher staging; executable readiness;
visible build/command/install errors; initial Off matching UI; --off removing
inherited native activation without optional helpers; status/diagnostic storage
failure no longer blocking the game; option-aware passthrough after removal;
idempotent installation preserving rollback. CreateDevice fallback now restores
the consumed downstream loader LINK_INFO cursor (34be414). Those are reproduced
code defects, not an assertion that the owner's crash cause is established.

Software evidence: **1409 backend tests, 1 root-only skip; 123 browser screens;
17 full-pipeline checks; 13 exact extracted-package checks**, all passing.
The extracted backend stages the real production ELF; initial Off, x2/x3,
read-only Vulkan12 features, Proton present metadata and recovery run through
software WSI with zero validation errors. Five checks against old internal.6
fail intentionally and are retained as negative controls. Production ELF needs
GLIBC2.17, loads under actual 2.31 and rebuilds byte-identically from included
source. The new CI installed-GSF check also passed locally. Remote CI is not
claimed successful. Saved environment install_script/start_skill now select
this integration checkout; refresh was executed, draft saving is not publishing.

Hardware/product acceptance remains **OPEN / NOT TESTED**: no /dev/dri or
configured authenticated Deck endpoint. No physical 30→90, SteamOS/Gamescope or
all-games-fixed assertion. G1 copy still follows G2 compute on the app queue;
missed renderer cut and HDR PQ quality remain explicit barriers. No stable
release/main push or renewed algorithm/PSNR research was performed.

## Withdrawn internal.6 — do not install

Source commit:f7d007998f137697d1a186af1afc79c3255a6ef8.
- Integrated Decky:docs/fg3/artifacts/GFG-Extreme-GSF-0.0.0-fg3-internal.6.zip
- Component:docs/fg3/artifacts/GFG-Native-FG-v0_0_0-fg3-internal_6.zip
- Checksums:docs/fg3/artifacts/SHA256SUMS.txt
- Acceptance:docs/fg3/evidence/gsf-package-1_1-acceptance.json

Actual extracted archive installed, launched a Vulkan WSI diagnostic with VVL0errors,
produced pairs.jsonl/report.md, removed retired status and rolled back successfully.
Rebuild from the included source produced byte-identical production ELF.
These are installable experiments, not a stable release or OLED90FPS acceptance.

## P0 implementation 1.1 — source checkpoint for PR133

The baseline architecture is retained. Inventory covers the actual237-file diff
at5782aa9 (not the stale-main411-file estimate). Source-time pair scheduling,
independent G1/G2 admission, x2 midpoint/x3 thirds, exact GPU RGB duplicate gate,
per-output control-header readiness and bounded age-preserving worker are implemented.
Display-budget exclusions suppress the corresponding GPU output; G2-only uses one
reconstruction at2/3 with a valid V+2 timeline signal.

Native GSF is a separate opt-in UI/RPC/module/launcher, with checksum-verified
component staging and Off/2/3. No new power control; no hidden foreign FG fallback.
GOOGLE timing is provisioned only when advertised, private/app namespaces stay
separate, refresh can be queried, and GFG is explicitly above FROG Gamescope WSI.
Real Gamescope/compositor/DRM execution remains untested; packet tests are not
hardware proof. See NATIVE_GSF_IMPLEMENTATION_1_1.md and HARDWARE_ACCEPTANCE_1_1.md.

Software:17 full-path checks pass,183 core assertions, native contracts,62 targeted
backend checks and118 browser screens. An additional4 WSI checks pass against the
actual ABI-targeted production ELF (x3/x2/recreate/unknown metadata, VVL0errors).
Host ELF requiring glibc2.38 was rejected; a signed-package glibc2.31 SDK build
requires symbols only through2.17 and loads with the actual2.31 loader.

The90Hz lavapipe trace is a negative performance result, not the product target:
45real/28generated WSI acceptances, no physical scanout.26 G1 skips retained a G2;
5 lower-refresh cases dispatched/presented G2 alone with zero validation errors.
Original failures and before/after traces are preserved in the1.1 evidence ZIP.

C hardware and D practical acceptance: NOT TESTED. No /dev/dri or connected Deck
endpoint. No stable release or claim of30→90OLED. Next work requires actual
SteamOS/Gamescope timing/DRM and optical uniqueness/latency evidence; inspect
single-queueG1-copy-afterG2, real-game contention, missed renderer cut and PQ quality.
No new algorithm comparison or PSNR sweep is authorized merely to fill this gap.

# Architecture accepted; GSF native implementation — 2026-10-16

The owner's latest P0 directive closes comparative research and prioritizes a
working full pipeline. No new PSNR experiment series. No release/merge/main change.

**One architecture:** GPU-resident two-source interpolation, shared hierarchical
motion analysis, adaptive geometric refinement, separate t=1/3 and 2/3 visibility,
bounded asynchronous output and per-output deadlines. GSF is the chosen backend;
full projective fitting is a GPU confidence-triggered path within that backend.
No user architecture choice, proprietary DLL or game depth/MV requirement.

Accepted decision, confidence, alternatives, component ownership and exact next
engineering barriers: docs/fg3/ARCHITECTURE_DECISION_1_0.md.
Closed experiments, including failures: docs/fg3/SHARED_FLOW_PROTOTYPE.md and
`docs/fg3/evidence/gsf-research-20261016.zip`.

Implemented code:
- 26416e2: poor affine agreement triggers full GPU projective fit; fast-turn
  probe recovered from −0.426 to approximately −0.002 dB against GPU LMC.
- de48b0d: app GPU capture → borrowed-device GSF → G1/G2 → async G1/G2/R WSI.
  Per-output absolute deadlines, one-source lookahead, late resync, bounded
  readiness/acquire waits, explicit deadline drops, rate-dependent stale budget.
  Pacing waits run only in the worker; metadata fallback stays synchronous.
  Coherent launcher: scripts/run_gfg_gsf.sh, default target 90 host clock.
- 7108ce5: reproducible full-pipeline check script and per-image render-finished
  semaphores in the test app, fixing VUID-00067 with validation above GFG.

- 1151078 / 852b7ca: cut/hold endpoint copies excluded from native synthetic credit;
  real-only fallback when no output worker exists. Source 9522288 has the final harness.

**Verified software functionality, not Deck acceptance:** thirteen full-path checks
pass under fresh VVL, both layer orders: 182 core assertions, borrowed device,
independent actual G1/G2/R pixel/order/spacing observation, recreate, queue idle,
unknown metadata, cut-credit exclusion and real-only fallback without worker. 31 Python regressions and both 168-check LMC baselines pass.
Historical fixture configured output clock9Hz, but its source3Hz description was wrong (the old app paced33ms); four real+six distinct synthetic with
~111 ms minimum host handoff spacing. 90 Hz software attempts missed deadlines and
are archived as failures. No interpolation/scanout success is inferred from CI.

Evidence: docs/fg3/evidence/gsf-native-acceptance.json and gsf-native-acceptance.zip.
Run: scripts/check_gsf_pipeline.py (native/core/validation paths required).
The native CI now has a separate GSF functional step and preserves its logs even
if later legacy checks fail. Latest remote CI success is not claimed.

Remaining barriers in implementation order:
1. Gamescope lower-MAILBOX/compositor-FIFO future-deadline contract. Current
   native slice supports ordinary FIFO; blanket FIFO substitution is not chosen.
2. Correct HDR linear/PQ processing, source/compute/presenter queue provisioning,
   actual game contention/power budget, and a cut heuristic that missed renderer cut.
3. Physical 30 real → 90 distinct OLED/SteamOS acceptance: Gamescope commits linked
   to DRM flips and preferably optical observation. Current cloud has no /dev/dri
   and no connected Deck endpoint. This is an explicit hardware blocker.

Do not promote this opt-in implementation to release or claim physical 90 FPS.

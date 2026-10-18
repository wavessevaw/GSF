# 2.1.5-gsf.10 — prerelease candidate, GitHub checks in progress

Use GFG-Extreme-2.1.5-gsf.10.zip for Decky or
GFG-Native-FG-v0_0_0-fg3-internal_10.zip for the standalone native component.
This fixes predictor compute-to-transfer ordering and Decky Off with conflicting
inherited XDG_CONFIG_HOME. The owner reports internal.9 launches; no measured
physical90 claim follows. See ../PRERELEASE_2_1_5_GSF_10.ru.md and
../evidence/prerelease-internal_10.json. Verify SHA256SUMS.txt.

The GitHub prerelease is published only after the dedicated remote checks pass;
these committed ZIPs alone do not establish that publication has happened.

---

## Previous internal.9 experiment

# 2.1.5-gsf.9 — latest experimental package, hardware NOT TESTED

Source: 7e8a9660a0c40e00275f87f0977c0758e6f3ebe3, PR #133.
Use **GFG-Extreme-2.1.5-gsf.9.zip** for Decky or
**GFG-Native-FG-v0_0_0-fg3-internal_9.zip** for the standalone component.
Includes all launch recovery and HDR PQ fixes from internal.7/.8.

G1 is copied/presented before G2 is submitted to the shared application queue.
Unneeded G2 reconstruction is canceled after a detected cut, duplicate or
expired deadline; final signals/fences still retire. Failed G2 submissions
cannot leave teardown waiting an unsent fence. x2 keeps its midpoint path.

17 pipeline checks, 7 HDR/legacy checks, 47 native Python tests and 14 actual
extracted-package checks pass. Production loads under actual glibc2.31 and
rebuilds byte-identically from included source. See ../DEFERRED_G2.md and
../evidence/deferred-g2-internal_9.json (raw results in matching evidence ZIP).
Verify SHA256SUMS.txt.

**Physical 30-to-90 OLED, real-game compatibility/cost/quality/latency: NOT TESTED.**
No connected Deck is available. The owner's all-game crash cause is not proven
on-device. The known missed renderer cut and game queue contention remain open.
This is an experimental package, not completed hardware acceptance.

---

## Previous HDR PQ package (superseded by internal.9)

# 2.1.5-gsf.8 — latest experimental package, hardware NOT TESTED

Source: 43658e884d67326183d3fce749840053a0c1cebe, PR #133.
Use **GFG-Extreme-2.1.5-gsf.8.zip** for Decky or
**GFG-Native-FG-v0_0_0-fg3-internal_8.zip** for the standalone component.
Both retain the launch repairs below and add linear-light GPU HDR10 PQ
reconstruction with retained swapchain color-space metadata. Numerical x2/x3,
1024-level duplicate precision, unchanged legacy shaders, the full GSF pipeline,
actual extracted-package WSI/recovery and exact-source rebuild checks pass.
See ../HDR_PQ_RECONSTRUCTION.md and ../evidence/hdr-pq-internal_8.json.

HDR game quality, physical output and GPU cost on Deck remain NOT TESTED.
The extra linear cache is15.625MiB at1280x800, only for PQ. G1/G2 queue ordering
and the renderer-cut issue are not claimed fixed by this color correction.
No stable release or physical30→90 acceptance is asserted. Verify SHA256SUMS.txt.

---

## Previous launch-recovery package (superseded by internal.8)

# 2.1.5-gsf.7 experimental packages — hardware NOT TESTED

Source: **9ddb5802e3cac33df0f5c50df6b67ff55d2d892a**, PR #133.
Includes upstream main **25ca625** (2.1.5, PR #140/#141 fixes).

- **GFG-Extreme-2.1.5-gsf.7.zip**: integrated Decky package. Its backend verifies
  and installs the native component and executable launcher at plugin startup.
  Initial mode is Off. Settings → Diagnostics → GFG Native Generator shows
  Off / ×2 / ×3, installation errors, build identity and the Steam command
  only after the executable/component are present. Restart the game for mode changes.
- **GFG-Native-FG-v0_0_0-fg3-internal_7.zip**: standalone independent component,
  full corresponding engine source, installer, rollback and optional diagnostics.
  Follow its README.ru.md; the standalone CLI explicitly opts in when no Decky
  mode marker exists. No proprietary DLL, model or other FG backend is needed.

Verify **SHA256SUMS.txt**. Full backend 1409 tests (1 root-only skip), frontend 123
screens, native contracts and 17 GSF pipeline checks passed in cloud. The actual
extracted ZIP passed 13 installation/WSI/recovery checks, including read-only
Vulkan features and Proton present metadata. The ELF rebuilds byte-identically
from its included source and loads with actual glibc2.31. Evidence:
../evidence/launch-recovery-2_1_5-gsf_7.json and the matching evidence ZIP.

**Steam Deck / Gamescope / physical 30→90FPS: NOT TESTED. Not a stable release.**
The cloud has no DRM device or authenticated Deck endpoint. The owner's all-game
exit cause remains unconfirmed on-device; the discovered launch defects have
independent reproductions and fixes. See ../LAUNCH_RECOVERY_2_1_5_GSF_7.md.

**internal.6 is WITHDRAWN.** Its old archives remain only as reproducible history.
They used the stale 2.1.2-te.8 plugin base and had launcher-installation/off/recovery
defects. If a Steam prefix itself fails, remove the entire GFG prefix from launch
options (empty field or `%command%`); `--off` requires a working executable first.

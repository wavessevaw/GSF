# KEDAVRA Decky plugin

One zip installs and controls everything. Frame generation itself runs inside the
patched Gamescope (`kedavra/gamescope`) that the zip carries in `runtime/`.

* **Install / remove** (backend runs as root): copies the runtime (with its own glibc and loader) to
  `/var/lib/kedavra/runtime`, checks its libraries and `--version`, grants CAP_SYS_NICE
  and enables `kedavra-gamescope.service`, which bind-mounts it over `/usr/bin/gamescope`
  at boot. The system image is never modified. A boot whose KEDAVRA Gamescope never
  writes its status is rolled back automatically on the next boot.
* **Diagnostics**: SteamOS and package versions, missing libraries, which gamescope is
  running (compared by inode), CAP_SYS_NICE, status freshness, previous automatic
  rollbacks; a report file with the boot unit and Gamescope journal.
* **Engine status** comes only from the status file the engine rewrites every second.
* **On/off and x2/x3** via the Gamescope convar `kfg_multiplier`.
* **Pacing evidence**: KMS page-flip trace (`kfg_trace`) analysed with `kfg_pacing.py`.
* **On-screen HUD** (`kedavra_hud.py`): rings with the frames that reach the screen (KMS page
  flips, generated frames included), the game's own fps, the multiplier and the share of new
  frames. Rendered here once a second into `/dev/shm/kedavra-hud.raw`, drawn by the KEDAVRA
  Gamescope as a layer (`kfg_hud`). The look follows GFG Extreme's ring HUD. Glyphs: Inter
  (SIL OFL 1.1), atlas built by `tools/make_hud_font.py`.
* **GPU benchmark** of the interpolation engine (`bench/kfg_vktest`).

Build: `sh package.sh OUT.zip RUNTIME_DIR BENCH_DIR` (CI job `plugin` produces the full
`KEDAVRA.zip`). Tests: `python3 -m unittest discover -s tests`.
# Experimental C1 build

All runtime installation and controls are in the plugin. Install this ZIP through Decky's
standard plugin interface, then open **Setup → Install / Reinstall C1 test runtime → Reboot now**.
Choose **Image filters** in Setup and use **C1 direct history** for A/B/A (Off / On / Off).
C1 and filters default to Off. The plugin saves both settings and restores them when Gamescope
restarts. Older/stale runtime status cannot enable C1; reinstall/reboot is offered in Setup.
**Remove and restore stock Gamescope → Reboot now** restores the system runtime.
The footer says **C1 TEST**; no terminal commands or file edits are required.


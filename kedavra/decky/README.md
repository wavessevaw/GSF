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
* **GPU benchmark** of the interpolation engine (`bench/kfg_vktest`).

Build: `sh package.sh OUT.zip RUNTIME_DIR BENCH_DIR` (CI job `plugin` produces the full
`KEDAVRA.zip`). Tests: `python3 -m unittest discover -s tests`.

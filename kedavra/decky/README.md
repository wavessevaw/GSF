# KEDAVRA Decky plugin

Control panel for KEDAVRA frame generation, which runs inside a patched Gamescope
(`kedavra/gamescope`). The plugin never generates frames itself.

* **Engine status** comes only from the status file the engine rewrites every second
  (`$XDG_RUNTIME_DIR/kedavra-status.json`). A missing or stale file is shown as
  "not running"; a setting is never reported as a working engine.
* **On/off and x2/x3** set the Gamescope convar `kfg_multiplier` through `gamescopectl`.
* **Pacing evidence**: "Record pacing trace" sets `kfg_trace`, so the engine logs the
  KMS page-flip time of every vblank it drives; stopping runs `kfg_pacing.py` and shows
  P95/P99 interval error, missed slots and every check.
* **Log** shows the Gamescope journal lines.

Build: `sh package.sh` (needs Node 20+). Tests: `python3 -m unittest discover -s tests`.

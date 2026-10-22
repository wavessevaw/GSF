# KEDAVRA in Gamescope

Frame generation and presentation inside the compositor (see
`docs/kedavra/PHASE_B_ARCHITECTURE.md`).

* `src/kfg/kfg.{hpp,cpp}`: ring of source frames, per-source analysis and per-vblank
  generation recorded into the composite command buffer, scheduler glue, KMS-flip trace,
  status file, convars.
* `hooks.patch`: the Gamescope side, against `ValveSoftware/gamescope@aa289ea`:
  * `steamcompmgr.cpp`: `kfg::OnPaint` / `kfg::OnPresented` around the connector present in
    `paint_all`; repaint on every vblank while generation is on;
  * `rendervulkan.{hpp,cpp}`: six compute shader types; `kfg::RecordComposite` before the
    upscalers in `vulkan_composite`;
  * `Backends/DRMBackend.cpp`: full composite while generation is on; `kfg::OnPageFlip`
    with the kernel flip timestamp;
  * `meson.build`: sources and shaders.
* `apply.sh <gamescope checkout>`: applies the hooks and copies the KEDAVRA sources
  (scheduler, pass plan, shaders) into the tree.

Build (Arch Linux, as in CI):

```sh
git clone https://github.com/ValveSoftware/gamescope && cd gamescope
git checkout aa289ea5977d8b21b92b0f246f0e8f0c225d180f && git submodule update --init
sh /path/to/gsf/kedavra/gamescope/apply.sh .
meson setup build --prefix=/usr --buildtype=release && ninja -C build   # /usr: SteamOS display scripts
```

Convars (`gamescopectl <name> <value>`):

| convar | meaning |
|---|---|
| `kfg_multiplier` | 0 off, 2 = x2, 3 = x3 |
| `kfg_trace` | CSV path: one row per KMS flip while generation runs; empty stops |
| `kfg_status` | JSON status path (default `$XDG_RUNTIME_DIR/kedavra-status.json`) |
| `kfg_debug_marker` | mark generated frames for camera checks |
| `kfg_budget_us` | GPU time per composite counted as over budget |

Limits: 8-bit SDR base plane only (HDR, 10-bit and YCbCr pass through), fixed refresh
(no VRR), the physical output only (no virtual connectors).

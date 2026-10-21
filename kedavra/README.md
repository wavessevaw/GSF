# KEDAVRA

Independent x2/x3 frame generation for Steam Deck OLED in Gaming Mode, generated and
presented by a patched Gamescope, controlled from Decky. No Lossless Scaling.

| Path | What |
|---|---|
| `sched/` | vblank-indexed scheduler (`kfg_sched.h`), unit and fuzz tests, compositor model `kfg_sim`, stress matrix |
| `shaders/`, `engine/` | interpolation shaders, shared pass plan, standalone Vulkan host and correctness/benchmark test |
| `tools/kfg_pacing.py` | pacing analyzer for model and KMS-flip traces |
| `gamescope/` | Gamescope integration (module + hooks) |
| `deck/` | probe, bench, reversible install, rollback |
| `decky/` | Decky plugin |

Docs: `docs/kedavra/` (phase A history, phase B architecture, phase C evidence, Deck protocol).

```sh
make -C kedavra test     # scheduler tests, model stress matrix, engine on a Vulkan device
make -C kedavra bench    # engine GPU timing (meaningful on the Deck)
```

Nothing here is verified on Steam Deck hardware yet; see `docs/kedavra/DECK_PROTOCOL.md`.

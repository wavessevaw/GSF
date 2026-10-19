# GSF Next: software verification report

This is lavapipe/llvmpipe (LLVM 15, two CPU worker threads), Xvfb/X11, Ubuntu 22.04, GCC 11.4; **not Steam Deck/RDNA2 performance**.
Output 640×480, half-resolution flow 320×240, five pyramid levels. Sixteen uploaded real frames per case, Vulkan validation, explicit GPU pixel readbacks and detailed timestamp output. This is a correctness workload with cold-start and CPU scheduling effects, not a real-time game benchmark.

All 12 integration cases passed; the display-slot planner independently checks the 60/90/120 Hz scenarios, changing refresh, hitches and fractional source cadence. The software WSI matrix uses a 360 Hz admission budget to ensure a 30 ms source interval can request every synthetic phase; Xvfb is not a 360 Hz physical display.

| Mode | Retired pairs with outputs | Pair p50 / p95 / p99, ms | Shared analysis p50, ms | G1 / G2 / G3 p50, ms | Built / WSI accepted |
| --- | ---: | --- | ---: | --- | --- |
| x2 | 13 | 29.629 / 247.906 / 495.502 | 19.495 | 9.360 / — / — | 13 / 13 |
| x3 | 15 | 44.119 / 56.778 / 59.501 | 19.986 | 10.430 / 11.007 / — | 30 / 30 |
| x4 | 15 | 52.376 / 128.720 / 133.214 | 21.565 | 10.679 / 9.617 / 9.717 | 45 / 45 |

44 generated GPU images passed temporal-position, unique-pixel-hash and temporal-order checks. The upstream centroid tolerance stays ±2 pixels; moving-block flow and HUD checks are retained. Empty display reservations are valid, so the harness checks actual reserved warm pairs instead of assuming that a particular source ordinal always generated an output.

Cases: ×2, ×3, ×4; queue limits 1/2/3; validation above/below the layer; pan; HUD; recreation; teardown without application idle; missing interpolator real-only fallback; source hitches; submit/DEVICE_LOST fault propagation. Constructed outputs must all retire into successful WSI acceptance in non-fault tests. Startup and unavailable-slot skips remain visible and are not counted as interpolation.

The split compute/present family path is implemented with per-queue host synchronization. Lavapipe exposes one shared queue, so independent RADV queue families, Gamescope FROG ordering, Steam CEF, suspend/resume, games, HDR, physical presentation, power, input latency and native game FPS remain unqualified.

The old Flow Lite 0.1.13 runtime is deliberately not loaded. No performance superiority over Flow Lite, ECO45, LSFG or MAKO is inferred from these CPU-driver timings. There are no comparative Deck runs or 10/15 W power results. Hardware acceptance follows DECK_PROTOCOL.ru.md.

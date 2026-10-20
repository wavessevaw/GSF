# GSF Next P0: adaptive output queue and bounded playout reserve

This is an experimental part of #58 / PR #59, not a performance-qualified release.

## Design

The previous **Предел очереди** stopped at three jobs. It now offers
`Авто` (`max_in_flight=0`) or manual `1..8`. For newly configured
games the default is Auto; existing profiles retain their selected depth.

Auto requests
`min(8, max(2, ceil(display_hz * multiplier / 90) + 1))`.
The extra one job absorbs normal scheduling variation above the theoretical
90 Hz minimum. For example:

| Display | Multiplier | Requested Auto | Playout reserve |
| --- | --- | --- | --- |
| 90 Hz | ×2 | 3 | >=4 refresh ticks (~44 ms) |
| 90 Hz | ×3 | 4 | >=5 refresh ticks (~56 ms) |
| 90 Hz | ×4 | 5 | >=6 refresh ticks (~67 ms) |
| 120 Hz | ×4 | 7 | >=8 refresh ticks (~67 ms) |
| 240 Hz | ×4 | 8 (cap) | >=9 refresh ticks (~38 ms) |

These are target scheduling holds, **not** measured image latency or proof
of physical scanout. The actual delay is also influenced by the game's
source-to-GPU queue, compositor, WSI and submitted work. Effective depth is
`min(requested, swapchain_image_count, available_private_ring_slots)`.
Expose `queue_effective`, `queue_capacity` and `queue_depth` separately.
If the application offers only 6 WSI images, an 8-job request uses 6;
**never** wrap the GPU ring before previous work is retired.

The private ring grows from 3 to at most 8 slots / 24 synthetic GPU images,
but is softly capped around 256 MiB by a format/extent estimate, with its
historical 3-slot minimum preserved. Actual Vulkan allocations may exceed
the estimate and remain subject to device memory availability. Extra WSI
images remain separately configurable (+3/+5); they must not be silently
raised beyond surface caps. Output/fence retirement still uses the same
ownership rules. The playout horizon grows to at least one more refresh tick
than the effective queue depth and is bounded at 9 ticks / 100 ms.

## Tradeoff and non-claims

The explicit latency trade is intentional. Deeper buffering does not fix
compute contention, missed refresh deadlines or uneven physical Gamescope
scanout by itself. It can reduce source game FPS through backpressure and
increase input latency, even when API Present gaps look improved. Comparing
visible scanout, real FPS and source-to-real present p95/p99 is mandatory.

The early-ingest separation from PR #59 stays off unless explicitly
enabled for controlled tests. Real WSI ownership and all submitted jobs
must be preserved under max queue, resize/recreate and failure teardown.

## Tests

- `gsf_queue_depth` deterministic monotonic scaling and resource clamps.
- C live-control accepts Auto/8 and rejects >8 or malformed values.
- Decky profile transport/selection covers Auto/8.
- Vulkan matrix exercises queue 8 and Auto at 144 Hz alongside original
  pixel golden and order tests.
- Steam Deck: hold constant game, scene, TDP, refresh and multiplier;
  compare 2/3/Auto/6/8, record source FPS, WSI intervals, p95/p99,
  presented synthetic acceptance and latency. Do not ship if native FPS
  collapses or the extra output is primarily discarded by WSI.

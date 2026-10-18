# AC Origins 0.1.2: generation enabled, deadlines missed

Observed session: `game-582160-20261018T143026Z-29twzpl7`, source
`303fab84033c00bcb9e67464aa11907cbbe26646`, 1280x800 RGBA8 UNORM,
mutable flag `0x4`, six swapchain images, FIFO, actual Gamescope refresh
11,110,617 ns. Gate accepted and the GPU bridge started. This is a different
failure from the preceding mutable-format refusal.

| Observation | Value |
| --- | ---: |
| Recorded source pairs | 7,546 |
| Source timestamp span | 195.259 s |
| Pairs dispatching one synthetic output | 2,474 |
| G1 accepted by WSI | 162 |
| G1 GPU-readiness deadline misses | 1,806 |
| G1 expired before readiness check | 504 |
| G1 acquire unavailable | 2 |
| Pairs with no synthetic dispatch | 5,072 |
| Completed GPU timing samples | 2,472 |
| GPU first-output ready time, median / p90 | 10.260 / 12.921 ms |
| Worker start minus source timestamp, median / p90 | 4.534 / 47.573 ms |

Most accepted synthetic outputs (159) occurred during the first 20 seconds.
The full-run source timestamp average includes fast menus; it does not
describe the slow gameplay. GPU timestamps measure the submitted work under
that run's contention, not an isolated LMC benchmark. WSI acceptance and
GOOGLE compositor feedback do not establish unique physical scanout.

## Concrete defect and bounded fix

The clock plans synthetic slots from host source-present intervals. The worker
has one executing job and one pending job. Admission can block the app when
the pending slot is occupied. CPU worker separation does not isolate the GPU:
the app, source copies and generation share its queue/device.

Previously `te_submit_frame` dispatched the planned mask regardless of elapsed
worker/ring/source-submit time. `present_synthetic` tested expiration only
after dispatch. Its first check also used the nominal target instead of the
earlier GOOGLE handoff/readiness deadline (target minus one quarter refresh).

Immediately before TE submission we now remove already expired outputs using
that same effective deadline. On the same-device game path, a fully expired
pair performs a zero-output history update, preserving its timeline signal
and real presentation. G2-only admission retains its 2/3 interpolation time
and compact output index. A new `compute_admission_ns` telemetry field records
the actual check; `expired-before-dispatch` distinguishes this rejection.

This does **not** predict GPU completion, eliminate shared-queue contention,
smooth the source clock, or prove the slideshow fixed. Work with a positive
remaining budget can still finish too late. Further changes need to address
available GPU budget and irregular source delivery, rather than assume a
fixed source FPS or change the user's game limiter.

## Reproduction without another Deck run

`tests/fixtures/ac-origins-deadline-slack.csv` contains all 7,546 rows as
relative effective-deadline slack at worker start, planned mask, old dispatch
mask and WSI acceptance. No game pixels or absolute process timestamps are
included. Source archive SHA256:
`0a28d3d7998bff6ba5842b3f0b7a9c47e5db9ea404543827e8a14205d6b56f34`.

Extraction from each `event=pair` row:

```python
slack = row['target_ns'][0] - row['refresh_period_ns']//4 - row['worker_start_ns']
fields = slack, row['planned_mask'], row['compute_mask'], int(bool(row['wsi_accepted_ns'][0]))
```

The production admission helper rejects **303 old dispatches already expired
at worker start**, retaining all 162 accepted G1 rows. This is a conservative
lower bound: the old log lacks the exact pre-dispatch timestamp, and more
deadlines can expire during reservation/copy submission. Replaying fixed
timestamps cannot predict how removing GPU work changes future scheduling
or recover a counterfactual FPS.

Run `make -C engine/gfg-native frame-clock`. It runs this trace plus integer
ratio, jitter, changing-rate and 2x/3x boundary tests. `gpu-ring` additionally
tests the production TE packet builder for zero-output history signaling,
G2-only compaction, two-output deferral and unpaced operation. CI also builds
and runs the extracted plugin/candidate with actual Vulkan/LMC on software
Vulkan; that is not a Steam Deck performance result.

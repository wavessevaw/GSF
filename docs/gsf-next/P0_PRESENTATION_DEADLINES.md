# GSF Next P0: real-priority presentation deadlines (experimental)

Source: Alpha 4 Deck report GSF-Next-582160(2).zip, build c92ea291.
This is a targeted scheduling safety change, **not** a fully decoupled real-frame
GPU path and **not** a qualified 45->90 Deck result.

## Failure mode
Alpha 4 emits a pair only after its GPU fence retires, then rebases every late
synthetic to a future absolute display-grid slot before the real B endpoint.
In the supplied trace, many real endpoints are consequently delayed by optional
synthetic output. More warps-per-second does not make output intervals uniform.

## Policy
- Real endpoints retain ordering, semaphore wait, present-id, pNext, and their
  original target grid slot; they are never dropped by this policy.
- Before acquiring a WSI image, skip optional synthetic output if its planned
  slot is > half a refresh late, if its target would collide with B, or if a
  newer real job is already queued. No catch-up rendering of stale history.
- If a WSI image has already been acquired for a synthetic, complete its
  presentation and retirement; dropping it afterwards would violate ownership.
- Existing half-tick minimum API spacing and full-tick target separation remain.
- Dropped synth counts include intentional deadline skips; dedicated
  `expired_synthetic` and `superseded_synthetic` split them from WSI starvation.
- Set `GSF_NEXT_REAL_PRIORITY_PACING=0` for the unchanged Alpha 5 scheduling
  baseline. Default in this experimental branch is enabled.

## Limit
The real B signal currently follows the shared optical-flow GPU submission.
Even after dropping stale synthetic presents, B may still wait on that job's
compute. True independent real-frame latency needs an earlier, separately
signalled frame-ingest boundary and verified WSI image retirement. Do **not**
claim it is already solved.

## Acceptance
Test against the same Steam Deck scene, 90 Hz and 45-source limit, compare
enabled vs rollback and Off. Inspect `present.jsonl` unique real ordering,
p50/p95/p99 interval, source-to-real submit latency, deadline-drops,
accepted synthetic ratio, WSI waits, and native source FPS. Track physical
scanout separately. A reduction in accepted synth alone is NOT proof of smooth
90 Hz; failing to improve real P95 or eliminating generation rejects the patch.

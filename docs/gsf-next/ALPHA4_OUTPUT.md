# Alpha 4: bounded detached output and source priority

The output path now has nine private device-local synthetic images, covering
three bounded work slots with up to three outputs each. One allocation backs
the images. Motion analysis and warp do not acquire downstream WSI images.
Only the output thread acquires a WSI image when a synthetic is GPU-ready,
copies it on the generation queue, observes the copy fence, then presents it.
The copy command has a separate pool from concurrent generation commands.
Per-WSI-image ready semaphores are reused only after reacquisition. Work-slot
reuse requires output retirement, including all copy fences. Allocation failure
returns safely to ordinary real-only presentation; partial teardown is idempotent.

At 1280×800 RGBA8 the nine raw images represent approximately 35.2 MiB; actual
allocation bytes are reported because alignment can increase the result. The
bounded job limit remains 2 by default, selectable 1/2/3. A deeper queue is not
forced onto existing profiles. There is an additional GPU-local copy per shown
synthetic. Its GPU timestamps, host wait and WSI acquisition wait are separate
from motion/warp timing. This trades some bandwidth/memory for removing early
WSI image pressure; no CPU pixel readback is used.

Alpha 2 could rebase a late pair while retaining empty historical slots between
the surviving synthetic and the real endpoint. The observed slower source then
increased requested work, causing feedback at high multipliers. Late sequences
now compact onto consecutive future slots. The presentation clock uses absolute
target ticks rather than `last_actual_call + period`, preventing wake jitter
from accumulating into a slower refresh clock. Buffered lag is quantized once
per job to the same grid. API calls can vary around a target: distinct target
slots stay a full tick apart, and a half-tick guard prevents catch-up bursts.
These are API timing contracts, not measured display scanout.

Before recording generation, the layer measures its own source-hook queue wait.
A wait over one quarter tick removes already-spent slot budget from the new
synthetic request. It never increases work because its own backpressure made the
source appear slower. This preserves the user-selected multiplier and records
`budget_skipped_synthetic` separately from built/ready/accepted/dropped outputs.
A synthetic which cannot acquire a WSI image within two refresh intervals expires;
remaining outputs in that pair expire together and the real frame gets priority.
No generated frame is relabelled as displayed or replaced by a history copy.

The running launcher now refreshes REPORT.json periodically; exporting during
play no longer leaves the initial NO_GENERATION_EVIDENCE result after WSI has
accepted synthetic frames. The UI retains the simplified layout and adds copy,
WSI-wait, private-memory and pre-GPU admission counters to diagnostics.

## Verification scope

The 640×480 Vulkan/pixel matrix retains its original ±2-pixel tolerances, unique
output hashes, temporal ordering and ×2/×3/×4 checks. New cases exercise a
producer overlapping the presentation thread and partial output-allocation
failure with Vulkan validation. Existing recreation, semaphore, present-wait,
live-control and failure cases remain enabled.

The source-cadence harness waits only for its own upload fence, not QueueWaitIdle
which would drain the layer after each source frame. It records throughput acceptance against unchanged targets of
at least 94% of the requested source rate, at least 92% of requested output
throughput, at least 93% of admitted ideal synthetic count, full retirement and
no WSI starvation under available budget. Its standard software clock is dilated
by 3: actual source 15/10/7.5 FPS and API budget 30 Hz represent the scheduling
ratios 45/30/22.5 → 90. These are deliberately **not reported as measured 90 FPS**.
Functional CI requires complete real/synthetic retirement and no unsafe WSI
reuse, and records numeric throughput pass/fail separately. Absolute FPS on an
unqualified CPU driver is not promoted to a hardware release gate. Run with
`--require-throughput` to enforce every unchanged numeric target.
The 128×128 software workload still runs a minimum 128×128 optical-flow analysis.
One lavapipe worker leaves CPU time for presentation. Benchmark runs omit validation
cost; the separate overlapping correctness case uses validation. No old pixel or
WSI correctness threshold is reduced. A deterministic 90-Hz clock test checks
10,000 calls with wake jitter without accumulated drift.

`source_preservation.py --realtime --require-throughput` enforces the actual 90-Hz throughput thresholds.
On the CPU-only environment, optical-flow execution and host scheduling can
consume a substantial part of a 22.22-ms source budget; actual 90-Hz throughput
is not consistently sustained. Slowing the software correctness clock is not a
Deck performance result. Steam Deck/RADV native FPS, physical output, input
latency and power require the supplied hardware protocol. No 45→90 hardware
success or parity with LSFG/MAKO is claimed for this release.


A real-time development probe on this CPU driver measured 44.985 source FPS in
Off mode, then 36.733 source / 61.554 WSI FPS in ×2. Its timestamp log reported
24.443 ms mean GPU work per frame (including cold-start and host scheduling),
which already exceeds the 22.222-ms budget of a 45-FPS source before output work.
The probe therefore fails the real-time acceptance thresholds and is not called
successful 45→90. The end-of-run source estimate recovered to 45.2 FPS after
admission pressure subsided; this is a recovery sample, not the run average.
The clock-dilated correctness run measured actual source/output values of
15.000/30.000 in ×2, 15.002/30.000 in ×3 capped at 30 Hz, 14.812/28.896 in ×4
capped at 30 Hz, 10.000/30.000 in ×3 and 7.500/30.017 in ×4. Those values remain
software API rates and are not multiplied by three and presented as Deck FPS.


## Gamescope chain ownership (Alpha 4)

Managed SDR launches disable only the Gamescope WSI Vulkan layer in the game
process (`DISABLE_GAMESCOPE_WSI=1`, remove its enable gate, exclude the two known
WSI layer names from loader activation). The compositor, Gaming Mode, display
session, Steam overlay and unrelated requested layers stay active. No system
manifest is rewritten. If the WSI layer sits above GSF it sees source Present
calls only, while injected generated calls bypass it. Isolating that shim gives
one owner for the full output sequence instead of attempting an unqualified
mixed ordering. This policy is recorded in REPORT.json. The native path is SDR;
The managed path targets SDR: without the Gamescope WSI shim some HDR
capabilities may no longer be exposed. HDR/full-WSI integration still requires
a separately qualified owner. Environment
isolation is covered by backend tests; actual Gamescope scanout still needs the
hardware protocol. No MAKO/LSFG runtime or licensed components are used.

Budget pressure measures only time inside the full-queue condition wait. Mutex
acquisition or host descheduling without a full queue is not counted as output
backpressure, preserving complete ×3/×4 requests when the queue has capacity.


QueueWaitIdle now drains already-returned Present requests owned by the layer
before locking and idling the relevant downstream queue. Previously it could
return while asynchronous generation/presentation remained queued; consequently
the ostensibly serial pixel harness accidentally exercised source backpressure
and correctly failed its three-output requirement. The fix restores the Vulkan
queue-idle contract instead of reducing that pixel/phase requirement. The
pipelined source case deliberately waits only for its own render fence, so it
continues exercising overlap and admission. Applications serializing every
source behind queue idle or physical present completion can still incur added
interpolation latency; an independent virtual-source completion contract would
be needed to remove that restriction.

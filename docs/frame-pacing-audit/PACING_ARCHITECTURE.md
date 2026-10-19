# Immutable scheduling and recovery

The live normal path still has one serial output worker per swapchain. Source
admission is bounded and nonblocking; a busy source ring forwards the native
frame. Every pair first clears `pace_real`, polls completed TE work without a
GPU wait, and snapshots its clock. Phase follows the reserved slot between
planned A/B endpoints. The opt-in direct-WSI adaptive policy now uses that same
immutable baseline: it cannot extend B or move a generated image after readiness.
Visibility/recovery retries no longer override an expired slot or bypass output
pacing. Two observed GPU-ready AND WSI-accepted generations establish STABLE.
Normal x2/x3 emit `pacing_recovery`; the shadow classifier does not control this.

The explicit x2 state cycle is STABILIZING → STABLE → CONGESTED → RECOVERY_PROBE
→ STABILIZING. Three dispatched misses cause congestion; a forecast that cannot
fit also suspends optional work. Backoff is 0.5–2s. A probe may test a stale cost
forecast but never changes the original deadline. Normal x2/x3 admission also
keeps its existing bounded 2s learning retry. No base-FPS threshold is introduced.
Polling now continues on budget/pressure bypasses, so completed samples can
refresh old estimates. A clock discontinuity reestablishes history.

`pacing_owner_plan.h` is a separate CPU reference policy. Tickets carry
source A/B, swapchain generation, sequence, logical slot, compact destination,
phase, target and deadline. Lifecycle is RESERVED → DISPATCHED → READY →
WSI_SUBMITTED → ACCEPTED → FEEDBACK, or EXPIRED. Once dispatched, the phase and
deadline never change. Feedback in the event model is simulated, not panel data.

A production buffered replacement needs independent bounded source snapshots,
generation destinations and presentation resources. Source acquisition may
retire after capture into retained history; output resources retire only after
their own copy/WSI completion. Acquiring an output too late requires a proven
image-release path; consuming its semaphore alone does not release the image.
All host queue access must remain serialized with DXVK/VKD3D submissions. A
native frame cannot silently disappear when optional generation is dropped.

The isolated TEST_LMC_OUTPUT_OWNER alternative exercises real installed Vulkan
loader dispatch, a virtual source image pool and ONE downstream real WSI. TE
retains two private inputs; a worker copies actual LMC outputs to the sole WSI.
The verifier GPU-copies two source snapshots and the output into a host buffer
in test code only. Every pixel is checked against the requested phase and must
differ from BOTH inputs. x2, x3 and x3 G2-only tau=0.5 are separate cases; copying
B in place of the generated image is an explicit negative control.

This prototype is not the buffered production replacement. It blocks its own
worker for retirement, has only one borrowed graphics/present queue family,
uses uniform grayscale test sources, and reanchors its output pacer on lateness.
It proves GPU generation, ownership transport and WSI ordering; it does not
prove a real-time immutable schedule, optical-flow quality or game compatibility.
It is excluded from the normal Decky payload. Migration remains gated on those
missing properties and G3. Enabling the test macro in a game is not a safe fix.

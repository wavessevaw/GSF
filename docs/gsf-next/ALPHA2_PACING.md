# Alpha 2: pacing and live controls

Alpha 2 reserves both quantized real endpoints before assigning synthetic display
slots. A constructed 46-source-FPS / 90-Hz / ×3 sequence previously admitted about
90 synthetic plus 46 real API presents per second; the corrected planner admits
about 44 synthetic plus 46 real. A 60-second invariant now covers every integer
source rate from 15 to 89 FPS, ×2/×3/×4, together with exact 22.5/30/45 FPS target
pairs, refresh changes and phase/order checks.

The presenter observes its GPU fence before calling downstream WSI. Ready delay
rebases the whole G1/G2/G3/B sequence; no common expired deadline collapses its
frames. A minimum monotonic call interval caps real plus synthetic API traffic.
Pacing adds roughly one source period of buffered delay and retains a bounded
1/2/3-job queue. It does not manufacture displayed FPS or eliminate backpressure.
The application-semaphore handoff runs on the original queue, keeping waits for
future game rendering off the asynchronous queue processing older interpolation.
Off submits an empty synchronized handoff without capture, flow or synthesis.

Actual Vulkan/X11 tests on lavapipe (LLVM 15, two CPU worker threads, Xvfb),
640×480, validation enabled, passed all 12 existing WSI/pixel cases and three new
pacing/control cases. The new tests inspect every actual API call timestamp,
require minimum 11.111112 ms at 90 Hz (16.666667 ms at 60 Hz), GPU-ready ordering,
increasing t, real-last ordering, and built = WSI-accepted retirement. A 45-ms
injected ready delay exercises overload without catch-up bursts. The live test
applies eight revisions in one process: ×2 → ×3 → ×4 → Off → ×2/Quality →
×4/Performance/queue 1 → ×3/images +5/Keep → ×4/60 Hz, including Acquire-driven
swapchain recreation. All revisions are acknowledged in the downstream trace.

The local run recorded minimum enabled call gaps of 11.131 ms in the 90-Hz ×3
case, 11.195 ms with injected late readiness, and 11.186 ms across live changes.
These are software API timing checks, **not Steam Deck GPU benchmarks, measured
90-Hz scanout, power results or proof of improved perceived smoothness**.
The regenerated SOFTWARE_REPORT.md covers the existing timestamp/pixel matrix.

Decky writes a per-game atomic control revision and reports pending/applied state
from the Vulkan layer. Image count and present mode require application handling
of standard OUT_OF_DATE recreation; other controls apply at a worker boundary.
Updating from Alpha 1 requires one game restart to load the new native library.
Unarmed/uninstalled profiles launch without the layer; prepared Off profiles
keep it resident for live enabling. Manual display refresh selection remains
necessary. Hardware acceptance and camera verification follow DECK_PROTOCOL.ru.md.


The Decky panel now defaults to game selection, a single applied/pending status,
three everyday controls (mode, quality, display refresh), and separate game/WSI
rate cards. Launch instructions, output tuning, and diagnostics are expandable
using native Decky buttons. The command remains visible/selectable if clipboard
access fails. Bundle-level React tests cover collapsed controls, advanced
expansion, persistence, Off, live acknowledgements and the display-cap message.

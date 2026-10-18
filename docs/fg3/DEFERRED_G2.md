# G1 transport before G2 submission — internal.9

The shared application queue previously received analysis, G1 reconstruction and
G2 reconstruction before the native bridge could submit the G1 image copy.
G1 consequently depended on work for a later display slot. Merely moving the
copy is insufficient: the lower WSI QueuePresent call can append a driver
synchronization submission to that queue, also putting G1 behind G2.

For borrowed-device GSF x3 pairs with two admitted outputs, the temporal engine
now submits analysis and G1, with the G1 timeline value but without the final
pair fence. The bridge copies and presents G1, then submits G2 after the lower
QueuePresent call returns. G2 retains its independent later deadline; a skipped
G1 does not automatically discard G2. Single-output admission and x2 retain
their existing submissions.

When the completed analysis identifies a duplicate or cut, or the G2 deadline
has already expired, the bridge cancels the unsubmitted G2 reconstruction.
An empty submission still retires the final timeline value and pair fence;
statistics and control expose only the output that was actually dispatched.
This removes wasted G2 reconstruction, not the already completed shared
analysis or G1 work. A skipped output is never counted as a displayed frame.

Poll/statistics and zero-time idle do not flush deferred work. Explicit resume,
blocking idle, the next pair and destruction flush it. A failed final submission
has no submitted fence: cleanup waits only the known G1 timeline value, returns
the submission error and does not hang on an impossible G2 completion. The
native bridge disables generation on failure and retains the accepted G1 result.

## Checks

The borrowed-device Vulkan test reads correct G1 pixels while G2 is still
unsubmitted and its timeline wait times out. It then checks explicit resume,
cancellation with one-output statistics/control, idle/destroy flushing, and an
injected G2 submission failure followed by cleanup and caller-owned device use.
The GSF pipeline checks both Vulkan layer orders, x2/x3 pixels and handoff
order, cut/duplicate cancellation, recreation, idle and unknown metadata.
Pair telemetry records `deferred_g2` and `g2_submit_return_ns`; accepted G1 must
precede a nonzero G2 submission-return timestamp. Exact extracted-package tests
check the same ordering using the production ELF installed by the real backend.

These are software Vulkan and WSI results. They do not measure physical scanout,
game GPU contention, Deck latency, cost or power. The application queue is still
shared; no separate compute queue or hardware scheduling guarantee is claimed.
The renderer's previously missed cut case remains separate and unresolved.
Physical 30-to-90 on Steam Deck OLED remains **NOT TESTED**.

# Native FG3 implementation — NOT DELIVERED

Contract: [#132](https://github.com/wavessevaw/GFG-Extreme/issues/132).
Canonical integration: [#133](https://github.com/wavessevaw/GFG-Extreme/pull/133).
No physical Deck 30→90, latency, power or game-quality acceptance is claimed.
No public release or main merge is authorized. A short-lived Actions artifact
is allowed only as an experimental Deck test package requested in PR #133.

`GFG_NATIVE_FG=1 GFG_NATIVE_GPU_BRIDGE=1` selects GPU transport. Its default
application-device mode borrows the caller's device/queue/timeline, records via
one coherent next-layer dispatch, and submits every dependent signal before
WSI present. No full-resolution CPU pixel transport, separate-device memory
imports, or host timeline relay are needed. A bounded ring polls GPU fences;
no steady-state QueueWaitIdle/DeviceWaitIdle is used. BGRA and sRGB encoded
bytes are packed in shaders and copied without an extra gamma conversion.

Algorithms: `GFG_NATIVE_ALGO=tiles` is the correctness/cheap-motion path;
`lmc-gpu` uses dense two-sided motion search, GPU RANSAC/IRLS motion planes,
motion palette, HUD history, occlusion/disocclusion handling and synthesis at
1/3 and 2/3. It restores this project's GPU solvers from `7f445f2` while keeping
current coalesced passes and cost tiers. CPU LMC remains an offline reference;
it cannot submit from a fit worker to a borrowed application queue. GPU LMC
has no model readback, CPU fitter or delayed second-half submission. Its
requirements include compute subgroup arithmetic, ≥512 threads/workgroup,
≥16896 bytes shared memory, and required storage/sample formats.

`GFG_NATIVE_GPU_SYNC=same-device` is the default and does not need external-FD
extensions because it borrows the application's device and queue. `shared-fd`
is explicit and will only start after capability checks, image/semaphore
imports, and a bidirectional timeline self-test; unsupported configurations
retain real presentation. The host-signalled
relay was removed because it reproduced VUID 03268. Shared-fd ownership and
CPU-split LMC signals still need independent synchronization validation.
Generation bypasses presents with unsupported pNext payloads so the app keeps
its exact present options, present IDs are rewritten only in a private copy,
and non-FIFO swapchains remain passthrough because MAILBOX can discard G1/G2.
Recovery never counts a duplicate as generated; unrecoverable acquired-image
failures forward the real frame and request swapchain retirement.

## Test package and limits

The internal Actions package is labelled `EXPERIMENTAL / NOT ACCEPTED`; it is
not a release. After extracting it, install with `bash install.sh`. Steam
launch options can use `~/.local/bin/gfg-native --diagnose %command%` for one
local diagnostic run. It captures allowlisted OS/Vulkan/DRM/sensor metadata and
the game's stdout/stderr, then writes a conservative report and a SHA-256
archive under `${XDG_STATE_HOME:-~/.local/state}/gfg-native/diagnostics/`.
Nothing is uploaded by the package. Review logs for private game/account data
before sharing. `bash rollback.sh` restores the exact previous files;
`bash uninstall.sh` removes GFG files and leaves a transparent launcher.
The package includes `BUILD-ENVIRONMENT.txt`, exact source commit/checksums,
and the corresponding Actions artifact includes validation logs.
The optional present-wait monitor uses synchronized timeout-zero polling when
the device supports the extension. Its timestamps describe Vulkan completion,
not Gamescope KMS or OLED scanout.

## Checks and limits

Core checks run with both CPU and GPU LMC. Borrowed-device tests cover motion,
BGRA output, static exactness and caller-owned timeline/device lifetime.
Native tests include both Vulkan layer orders, synchronization validation,
actual WSI-image pixel inspection, sRGB, failure injection and 1280×800.
Private package checks cover checksums, off mode, exact rollback/uninstall,
installed GPU execution and byte-identical rebuild from included source.
Raw logs and continuation are under `docs/fg3/`; they are software evidence.

The GPU binary excludes the old CPU pixel bridge. Its isolated regression
uses `ENABLE_CPU_BRIDGE=1` / `make wsi-smoke-te` in `build-cpu`. Native fallback
without the generator and the old no-DLL compatibility bootstrap stay tested.

**Remaining engineering is material:** asynchronous paced output, deadline/drop
policy, KMS/frame-ID correlation, acceptance traces/content hashes, and
hardware validation. The present-wait monitor is synchronized but does not
observe KMS scanout. Back-to-back FIFO handoffs are not claimed to guarantee
90 Hz. See [the SteamOS/Gamescope audit](../../docs/fg3/STEAMOS_DEEP_AUDIT.md).
The uploaded inventory has no authenticated test endpoint; actual device
results must not be invented. [Continuation](../../docs/fg3/CONTINUATION.md).

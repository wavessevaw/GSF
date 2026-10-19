# GSF Next architecture and compatibility

GSF Next 0.2.0 Alpha forks the open AFMF-Linux 1.3.1 host (MIT) and FidelityFX
Optical Flow, with exact provenance in `engine/gsf-next/UPSTREAM.md`. It shares no
runtime with GSF Test 0.1.13. The retained upstream identifiers `afmf_*` are
implementation names, not a claim to ship AMD's proprietary AFMF driver.

The application keeps real downstream Vulkan WSI images. At present, an adapter
submission consumes its binary waits once and signals a per-slot handoff.
A work thread captures the current image into a two-image GPU history, creates
luma/pyramid/scene-change state once, estimates motion once, then performs up to
three temporally parameterized warps. There are no mandatory pixel readbacks;
readbacks are only for the explicit image-test/dump configuration. Source images,
command buffers and uniforms remain held until the corresponding submission
fence retires. Output ready semaphores belong to individual WSI images.

The work queue may be compute-only; presentation uses the original present-capable
application queue with externally synchronized host API access. This is essential
when RADV exposes compute and presentation in different queue families. No host
mutex holds a GPU-readiness wait. A second thread presents reserved intermediates in increasing temporal order,
then the current real frame. It waits on GPU readiness independently of the game
hook, and bounds pacing holds to one source period from arrival. The presentation
queue is bounded to 1/2/3 jobs, default 2. Backpressure can still block application
present when that bound is reached; no claim of zero latency or zero game impact
is made. This is a real-WSI companion design, not the former virtual-source WSI.
Increasing extra WSI images (+3/+5) does not itself increase the job queue limit.

The display-slot planner derives temporal phases from one monotonic display
clock and actual captured source timestamps; it admits at most multiplier-1
outputs and avoids endpoint duplicates and loading gaps over 250 ms. At 90 Hz,
45→×4 reserves only one intermediate and 30→×4 normally reserves two. At lower
source rates, ×3 and ×4 can construct two/three genuinely different outputs from
one common flow. Actual free-image misses remain possible and are counted.
No full optical-flow search is repeated for another output. Per-output scratch
is reused with compute/transfer barriers; target images and ready semaphores are
independent. Quality reduction does not relabel the selected multiplier.

The profile refresh rate is **user supplied**, not a measured refresh rate.
Changing Steam's display refresh requires updating the profile and restarting
the game. Automatic refresh notification is not implemented in this Alpha.

Default Performance runs flow at half input resolution with five pyramid levels;
Quality runs full input resolution. Both reconstruct directly at output size with
bilinear motion, static HUD preservation, local disagreement fallback and shared
scene-cut detection. No proprietary model, game depth, game motion vectors,
DLSS integration, low-resolution synthesis/upscale pipeline, or perfect HUD
segmentation is claimed. Scene-cut fallback may blend real frames and is not
represented as newly recovered motion detail. Optional debug image dumps expose
all phases, allowing duplicate and temporal-order checks.

The layer does not force FIFO. Auto requests MAILBOX only where supported and
compatible with the copied maintenance1 mode list; Keep preserves the game's
present mode. MAILBOX acceptance does not prove that Gamescope displayed all
accepted frames. Real present ID/fence/mode metadata remains on the real frame.
Unknown presentation chains are drained and forwarded real-only before consuming
their waits. Unsupported interpolation or HDR colorspace goes real-only; there is
no history-copy generation fallback. First swapchain creation can retry the
original request after a modified request fails; an `oldSwapchain` request is not
replayed because the old handle may already be retired.

A failed work submit forwards the already-consumed application waits only through
a successful GPU signal. If both submits fail, it returns the error and retains
images for teardown; it never presents an unsynchronized image. Actual DEVICE_LOST
is not recoverable into normal rendering on the lost device. The automated fault
case checks propagation/no unsafe synthetic present, not transparent recovery.

Supported target: Linux x86_64 Vulkan 1.1+, SDR8 and color-buffer-only operation.
HDR colorspaces deliberately pass through pending numeric/metadata tests;
32-bit games, multi-swapchain generation, protected images and cross-process
compositor qualification remain unsupported/unqualified. The default is no
privileged queue priority, TDP changes, GPU clock changes or system manifests.

Decky installation is user-owned and transactional. The offline payload carries
checksums and corresponding source; relocated manifests pin the installed
library. Preflight failure runs the game once without the new layer. Disabled
profiles are strict pass-through. Runtime state is `~/.local/share/gsf-next`,
separate from old `gsf-test`; the plugin never imports old enabled profiles.

Optional detailed GPU reporting records one retired pair's source indices,
reserved phases, common analysis and G1/G2/G3 timestamp durations per record.
Files are separate per process/context and rotate at 16 MiB (one backup).
It is off by default; the normal panel still reports sampled GPU pair timing.
Timestamp-query readback is not a full-frame GPU→CPU transfer.

`vkWaitForPresentKHR` / `vkWaitForPresent2KHR` wait for the original ID to be
submitted downstream before asking the driver, with only the remaining caller
timeout. Zero timeout never waits for a queued worker. Host timeout/error
contracts are tested separately; driver physical-present-wait extensions are
unavailable on the software test device and remain hardware-qualified later.

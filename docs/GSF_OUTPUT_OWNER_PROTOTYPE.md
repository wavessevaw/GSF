# GSF: Output-owner transport prototype (2026-10-17)

**Development branch only. Does not install any Vulkan layer, change the stable
2.1.6 plugin, or claim that generation is working on the Steam Deck.**

## Evidence and why the old path cannot be treated as working

Four user-uploaded diagnostics from LAB.4: at 07:03 UTC Desktop x2
3,820 real and 1,125 synthetic *WSI handoffs*; 706 bounded-mailbox
backpressure events, 128 stale jobs and an output worker median 27.95 ms.
At 07:12 UTC Desktop x3 3,274 real and 2,247 synthetic WSI handoffs with
440 backpressure events. The user reports jerky motion and missing/real
frames. Physical scanout and uniqueness of intermediate frames were never
observed. Two Gaming Mode captures were real-only due to 10-bit Gamescope
protection (not a working frame generator).

The current layer copies the game's swapchain into temporal history and
independently acquires/presents generated images on **the same game swapchain**;
it subsequently presents the game image. Even when the API calls are ordered,
the FIFO WSI can block, creating a queue backlog that delays the source. It
is not a genuine separation of game rendering and final output ownership.

## Checked reference: MAKO, not hypothetical LSFG.dll internals

See [MAKO real_frames.cpp](https://github.com/eugeniosegala/MAKO/blob/main/engine/mako-render/src/swapchain/present/real_frames.cpp),
function `Swapchain::submitSourceCopy`. A real image is blitted into a
separate GPU `sourceImage`, with explicit layout barriers and timeline
signaling. See [generated_frames.cpp](https://github.com/eugeniosegala/MAKO/blob/main/engine/mako-render/src/swapchain/present/generated_frames.cpp):
MAKO acquires additional swapchain images, copies generated textures and
issues `vkQueuePresentKHR` before a final original present. This confirms
the MAKO Vulkan WSI integration but **not** the proprietary Windows
Lossless Scaling capture implementation.

## Architecture

```text
real input swapchain (game; NEVER independently displayed)
    |  capture real GPU frame and signal ready fence
    v
source cache: pinned A, B until retired by the owner
    |  A+B -> GPU interpolation -> true output-ready fence
    v
GFG output_owner (single authority)
    |  A (real) -> A½ (synthetic, optional) -> B (real)
    v
private output swapchain/window -> Gamescope -> physical screen
```

The private output surface is a **hard prerequisite**. Merely overlaying the
generated texture on top of a normally presented game image would create two
unsynchronized outputs. An external window also needs a supported frame
capture route in Gamescope, a no-recursion guarantee and compositor surface
prioritization. These capabilities must be proved rather than presumed.

### Implemented in this branch

`engine/gfg-native/src/output_owner.h` implements a Vulkan-independent
reference ticket contract:

* The first real capture A is held until B exists, then A/AB/B are
  scheduled on a shared ordered output lattice. Following real C yields
  BC/C, never duplicate B.
* Late/invalid GPU synthetic or scene discontinuity: drop **only synthetic**;
  schedule the next real frame promptly. No additional delay to try to
  salvage an outdated generated image.
* Bounded real display latency for generation, 90Hz budget checks and
  monotonic output timestamps, with a flush of the only real frame on exit.
* Caller retains **source image memory and readiness fences** until the
  owner has actually submitted and retired output tickets.
* Structured tickets contain input IDs and presentation due times.
  Tickets are not WSI calls, and successful tickets are not scanout proof.

`make -C engine/gfg-native output-owner` tests real/synthetic chronological
ordering, a 30FPS→60Hz case, recovery after a 836ms pause changing from
27→38FPS at 90Hz, missing/cut GPU work, a source rate too fast to generate
at 90Hz and single-frame flush.

## Actual one-output Vulkan test added

The test `tests/wsi_gpu_observer.c` has a separately compiled
`GFG_NATIVE_TEST_OWNED_WSI` path. It constructs a dedicated Vulkan
XCB swapchain (using one output window) under lavapipe / Xvfb; feeds
mock source frame IDs into the output owner; clears uniquely marked real
and intermediate full-screen images; submits them to Vulkan and calls
`vkQueuePresentKHR` from a **single owner only**. Each accepted WSI
handoff emits an auditable `kind/pair/due_ns` event. With
`GFG_NATIVE_TEST_ASYNC` it uses a monotonic 60-Hz output clock and
per-submit fences rather than forcing a GPU `QueueWaitIdle` on every
output. The CI workflow builds both the pure planner test and this
actual WSI smoke.

This proves API-level presentation is possible with one controlled
output surface; it does **NOT** yet take frames from a running game,
replace the game's original swapchain or measure real OLED scanout. The
'generated' colors in this test are deterministic synthetic test
textures, **not** a GSF neural/motion model.

**Why not assume a generic Wayland screenshot API?** Official
[Gamescope documentation](https://github.com/ValveSoftware/gamescope)
describes a compositing manager that may directly flip game frames over
DRM/KMS. Its embedded SteamOS session is not necessarily exposing
a wlroots screencopy protocol usable by ordinary clients. We must
prove a capture interface exists before relying on an external
window-capture design; a Vulkan layer that retains the original GPU
source image is the current known integration path.

## Additional Gamescope verification (October 2026)

Official [Gamescope compositor README](https://github.com/ValveSoftware/gamescope)
explains that embedded SteamOS can **direct-flip** game buffers through
DRM/KMS, and may composite with async Vulkan compute when needed. Merely
drawing a fullscreen overlay does not control scanout or make a stable
frame-generation output owner. Official
[Gamescope PipeWire implementation](https://github.com/ValveSoftware/gamescope/blob/master/src/pipewire.cpp)
exposes a PipeWire `Video/Source` node (potentially with DMA-BUF). It is
a compositor **output** capture: using it for GSF game-only input can
capture the generated output itself, creating a feedback loop; it also
requires frame-accurate timestamps and proof of supported node access
in the embedded Deck session. The
[Gamescope control protocol](https://github.com/ValveSoftware/gamescope/blob/master/protocol/gamescope-control.xml)
has one-shot `take_screenshot` operations, not an authenticated
high-rate per-game frame capture protocol.

[MAKO's WSI isolation guide](https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/WSI-ISOLATION.md)
states Gamescope the compositor and the optional Gamescope WSI Vulkan
layer are distinct, and notes layer-ordering and FROG WSI interaction.
This must be considered when connecting the private owner to a real
application, but disabling a WSI layer does **not** by itself make a
private output surface or prove GPU scheduling.

**Decision:** keep the original game's Vulkan Layer as the known
GPU source-capture location, but move *all visible output* to a
separate verified WSI surface. That requires: a valid way to return
original `VkAcquireNextImageKHR` images to game use without displaying
them; a private surface visible in Gamescope with game focus/input
preserved; cross-device/image-semaphore lifetime; no capture-recursion;
and experimental pixel/scanout verification. A hidden/unpresented
game-swapchain is **not** automatically safe because its acquired
images may never be returned to the WSI and the game will stall.

This branch's GPU test creates two offscreen **mock source images** in
one Vulkan device (never submitted to WSI), captures their contents,
copies real source images into one output swapchain, and sends
a split A/B composite instead of motion interpolation. GPU buffer
readback verifies actual pixels for both kinds. CI also checks long
pauses, GPU misses and real-only priority at different refresh rates.
The mock source is NOT a live game's swapchain and the split A/B
image is NOT a working temporal generator.

## Game-facing Vulkan WSI contract (next integration milestone)

**Opt-in software prototype on this branch, not attached to shipped
`libVkLayer_gfg_native.so`.** A virtual-source facade now models
`vkGetSwapchainImagesKHR` count/query/`VK_INCOMPLETE`, app acquire,
acquire-semaphore GPU signalling, app present with game render semaphore
consumed **exactly once** by a GPU snapshot submission, and release of
the game image after its copy fence retires. Unsupported `pNext` chains
are refused before consuming game semaphores; if copy submission fails
after semaphore ownership may have moved, the image stays quarantined
rather than replaying waits. The facade never submits the virtual
swapchain's handle to the Vulkan ICD.

The full Vulkan lavapipe test connects those API-shaped operations
to real GPU queue submits and an actual XCB WSI swapchain. It tests
the same game VkImage being reused every frame while GSF separately
retains two history images. The *only* `vkQueuePresentKHR` downstream
goes to the compositor output swapchain. This is closer to a game,
but the producer remains a controlled mock rather than a third-party
application; no layer hooks have been enabled.

**Important surface choice:** the visible owner can create its real
downstream swapchain against the **game's existing `VkSurfaceKHR`**;
it does not inherently need a second window or a private foreign
surface. The game could receive a virtual `VkSwapchainKHR` and
offscreen `VkImage` handles while the owner uses the original surface
for the *only* displayed output. No gamescope overlay priority games,
screencopy protocol or separate focus window would be required.
However, this is an integration proposal and does **not** yet work
with arbitrary games.

**Major compatibility blocker:** regular Vulkan swapchain apps put
their image into `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` before present,
whereas ordinary offscreen VkImages cannot be presented. See the
[Khronos image layout specification](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageLayout.html).
`src/virtual_layouts.h` is a narrow pure translator for **registered
virtual images only**: it maps PRESENT layouts to TRANSFER_SRC in
legacy and Sync2 image barriers and includes renderpass attachment
mapping behind an explicit exclusively-virtual gate. The software GPU
producer deliberately requests a PRESENT layout; the helper rewrites
it **before** submission to lavapipe. The real output swapchain's
PRESENT layout remains untouched.

This does NOT amount to hooking every app command: a real implementation
must handle `vkCmdPipelineBarrier`, `vkCmdPipelineBarrier2`/KHR,
`vkCreateRenderPass`/2, attachment/image-view/framebuffer provenance,
dynamic rendering, multi-swapchain calls, present-id/fence extension
chains, HDR10 output, window resize/`oldSwapchain`, image ownership
transfers and thread-safe semaphore retirement. Fail closed on any
unsupported shape, retaining the original game's valid WSI path.
No real-game compatibility claims are authorized yet.

### Virtual WSI ABI and bounded acquire semantics

`src/virtual_wsi_facade.h` now models the app-facing shape of
`vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`,
`vkAcquireNextImage2KHR` (single-device subset), and
`vkQueuePresentKHR`. It never forwards virtual handles to the ICD.
Acquire signals the app's Vulkan binary semaphore/fence through a
GPU queue callback. Present consumes game-render waits **once** in
the snapshot submission, then the game source image is released after
copy completion. Duplicate releases, unsupported pNext and failed
copy submissions fail closed.

With a busy source image and a nonzero timeout, an actual Vulkan game
expects Acquire to **wait** until it is released: the facade uses a
CLOCK_MONOTONIC conditional variable, supports finite timeout and
UINT64_MAX waits, and wakes blocked threads when GPU retirement completes
or the swapchain closes. This **requires an independent retirement
worker** for live asynchronous operation. The controlled Vulkan test
explicitly waits on the copy fence and retires the image; this is
not yet a fully asynchronous real-game presentation path. Unit tests
exercise threaded wake-up and closure; success requires CI to pass.

**No production Vulkan layer hooks** currently route real game calls
through this facade. Not all Vulkan extensions or device groups are
supported; do not silently enable the experimental route in the
2.1.6 plugin.

### Experimental Vulkan Loader interception (isolated test build)

Under the compile-time-only switch
`GFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS`, the existing
`src/layer.c` exposes virtual source interception via **both**
`vkGetDeviceProcAddr` and `vkGetInstanceProcAddr`:

* `vkGetSwapchainImagesKHR` returns app-visible offscreen VkImages.
* `vkAcquireNextImageKHR` and `vkAcquireNextImage2KHR` honor
  virtual image acquisition and GPU-signalled semaphores.
* `vkQueuePresentKHR` **redirects** app virtual presents to
  snapshot capture; the real output WSI is still passed through.
* `vkDestroySwapchainKHR` prevents fake virtual handles from being
  forwarded to downstream WSI; mixed app-virtual/output WSI presents
  are rejected **before** consuming any app semaphore.
* `vkCmdPipelineBarrier`, `vkCmdPipelineBarrier2`, and
  `vkCmdPipelineBarrier2KHR` rewrite PRESENT layouts **only** when
  the VkImage belongs to the registered virtual source; app-supplied
  commands are copied and never mutated in place. The actual downstream
  compositor image is never rewritten.

`tests/output_layer_dispatch.c` exercises this **actual native layer
code** with two injected fake swapchain registrations and a downstream
stub driver, including GIPA/GDPA identity, app source versus real
output separation, image ownership, barrier dispatch, mixed-present
rejection, and destruction. It is a stronger interface test than
calling `virtual_wsi_facade.h` directly but **still lacks a
real game's vkCreateSwapchainKHR interception and image allocation**.
Production builds do not define this macro.

Pinned Vulkan-Headers **v1.4.365** are required to compile the
existing full native layer (old distribution headers omit the latest
present-id2/present-wait2 and other extension types). Use the
specialized Actions workflow as source of truth for compiler and
UBSan results. The experimental code must remain disabled if any
check fails.

### Not yet implemented / hard merge gate

1. Private output swapchain/surface in SteamOS Gaming Mode (Gamescope), with
   the original input swapchain **prevented from displaying twice**.
2. GPU texture lifetime ownership, imported sync, app-semaphore consumption
   and full WSI swapchain recreation without deadlock.
3. Match game-source FPS after entering and leaving menus/loads without
   backlog; measure not just API counts but an external **pixel-sequence**
   observer. The observer must see colors A, A½, B, ... in physical order.
4. No dropped/stolen real frames and additional input latency inside a
   documented user-configured budget. Regression against LAB.3/LAB.4.
5. Gamescope 10-bit compatibility must clear independent Vulkan dispatcher
   integration tests. PR #151 fixes one borrowed-device dispatch bug, but
   the packed10 protection stays enabled until tested on actual hardware.

**Do not present this branch as a downloadable, working GSF release.**

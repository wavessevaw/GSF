# SteamOS / Gamescope frame-path audit — NOT ACCEPTANCE

This bounded audit answers what the available public source supports. It does
not establish that the Deck's OLED panel displayed 90 distinct frames. The
SteamOS 3.8.28 Gamescope package was not available to this executor, and the
reported device inventory does not prove the panel type or expose a remote test
endpoint.

## Image provenance and exact target versions

At 2026-10-16 00:55 UTC, `scripts/probe_steamos_sources.sh` requested a range of
at most 2,048 bytes from Valve's official recovery index and FAQ, both SteamOS
package mirror roots, and the public SteamOS GitLab host. Every connection
failed before an HTTP response with `curl: (56) CONNECT tunnel failed,
response 403`; the curl transfer count was zero. The evidence is in
[`steam-source-probe-20261016.txt`](evidence/steam-source-probe-20261016.txt).
No recovery image URL was selected, no image bytes were downloaded, and no
image hash, file size, image version, partition table, or extracted package
version can be reported. The owner's PR comment referenced a roughly 3.1 GiB
Steam Deck 3.8.14 recovery image; that is not an image inspected here and is
older than the reported installed system.

The user-run inventory at
[`device_inventory_20261015.json`](../../research/results/device_inventory_20261015.json)
reports SteamOS 3.8.28 build `20260922.1`, kernel
`6.18.50-valve2-1-neptune-618-gc7289a96b14d`, Mesa
`26.0.0-devel (git-d246f60ddf)`, RADV VANGOGH, and Vulkan API 1.4.330. It labels
the model/board Galileo but explicitly does not verify the optical panel.
Gamescope package/build, Gamescope WSI layer build, Vulkan loader build,
Xwayland/Wayland versions, and Steam/Proton/DXVK launch versions are absent.
Layer enumeration in that report is availability, not proof a layer was active.

The only source pinned and inspected is public upstream Gamescope commit
[`2b5cf191d1047d6895516a834de8a72ac1c342f3`](https://github.com/ValveSoftware/gamescope/commit/2b5cf191d1047d6895516a834de8a72ac1c342f3),
dated 2026-10-13. It is a useful source snapshot, not a match for the
SteamOS build timestamped 2026-09-22. Exact SteamOS source/package matching is
therefore **unverified**.

## Frame path and what the source establishes

```text
Proton / DXVK / Vulkan game
  -> game swapchain + vkQueuePresentKHR
  -> Gamescope WSI layer
       game-facing FIFO -> Gamescope swapchain protocol
       driver-facing swapchain/presents use MAILBOX
  -> Xwayland or Wayland commit queue
  -> steamcompmgr selects a FIFO commit for a window at a vblank
  -> Gamescope Vulkan compositor or direct-scanout route
  -> DRM/KMS page flip
  -> physical connector/panel                         [not observed here]
```

At the pinned source, `VkLayer_FROG_gamescope_wsi.cpp` explicitly sends
`VK_PRESENT_MODE_MAILBOX_KHR` to the driver while implementing FIFO itself over
the Gamescope swapchain protocol ([lines 1577–1593](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/layer/VkLayer_FROG_gamescope_wsi.cpp#L1577-L1593)).
Its `QueuePresentKHR` translates application presents into swapchain protocol
updates and requests driver MAILBOX for layer-owned swapchains ([lines
1881–1938](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/layer/VkLayer_FROG_gamescope_wsi.cpp#L1881-L1938),
[2018–2074](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/layer/VkLayer_FROG_gamescope_wsi.cpp#L2018-L2074)).
The underlying driver's MAILBOX mode alone therefore does not mean the game
requested or received MAILBOX semantics.

`steamcompmgr.cpp::handle_done_commits_xwayland` and
`handle_done_commits_xdg` keep a per-vblank set of FIFO window sequence IDs.
After one FIFO commit for a window is selected, later FIFO commits for that
window remain queued until another eligible vblank ([lines 8056–8153](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/src/steamcompmgr.cpp#L8056-L8153)).
That is FIFO pacing, not latest-frame coalescing. `window_refresh_cycle()` also
uses the limited app refresh cycle when Gamescope's target FPS applies to the
window ([lines 7915–7919](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/src/steamcompmgr.cpp#L7915-L7919)).
A 30 FPS limiter on the relevant window can therefore defeat 90 Hz FG pacing;
the target session must run at 90 Hz without a lower Gamescope app cap.

`vblankmanager.cpp::CalcNextWakeupTimeLocked` schedules against the output or
nested refresh interval and maintains draw-time/red-zone margins
([lines 133–185](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/src/vblankmanager.cpp#L133-L185)).
`rendervulkan.cpp::vulkan_composite` composes `FrameInfo_t` layers through the
Vulkan renderer ([around lines 4360–4545](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/src/rendervulkan.cpp#L4360-L4545));
the output swapchain is presented with a Gamescope-owned present ID and waited
on by its present-wait thread ([lines 3043–3070, 3105–3152](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/src/rendervulkan.cpp#L3043-L3070)).
This is compositor-output timing, downstream of the game's WSI presents.

## Does back-to-back G1, G2, R prove 90 distinct panel frames?

No. The source supports this conditional statement: if the game-facing
swapchain reaches the Gamescope WSI layer as FIFO, each accepted FIFO commit
for the same window is retained for a later eligible vblank. So a producer
which queues G1, G2, and R can have those commits selected on consecutive
vblanks, provided all are ready and no per-window FPS limiter lowers the
refresh cycle. The renderer may use a compositor or a direct-scanout path.

This does not prove 90 distinct images on the OLED. It does not establish which
Gamescope layer order the packaged Deck actually used, whether the native FG
layer is above the Gamescope WSI layer, whether each commit reaches scanout on
that build, whether 90 Hz is active, or whether the model-reported Galileo is
the OLED device. If GFG is below Gamescope WSI, it may see a driver-facing
MAILBOX request and bypass generation; the layer must be observed in the actual
Deck chain. Safe FIFO refusal is preferable to emitting into an unknown
MAILBOX path.

Gamescope's Wayland `pixel_out` feedback is passed from
`lastCommit->predicted_present_time` (`steamcompmgr.cpp` lines 8204–8206), so it
is a prediction, not the final KMS event. `DRMBackend.cpp::page_flip_handler`
receives the kernel page-flip timestamp (`sec/usec`) and marks a vblank; it
updates the visible framebuffer set, but the source still has a TODO to map a
queued framebuffer to an in-flight event when more than one is in flight
([lines 772–804](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/src/Backends/DRMBackend.cpp#L772-L804)).
That callback is the strongest available compositor-side timing hook, but this
snapshot does not expose a complete FG present ID → framebuffer ID → KMS flip
record. An external high-speed optical capture is still needed to confirm
distinct pixels actually reached the OLED.

## Falsifiable Deck test and acceptance design

Run the packaged candidate at 1280×800 with the OLED confirmed at 90 Hz. A
deterministic 30 Hz test scene must put a changing, pair-coded marker in R, G1,
and G2. Record for each output: source pair and kind, GPU completion timestamp,
queue handoff timestamp, Gamescope FIFO commit, DRM framebuffer ID, CRTC page
flip timestamp, and optical frame observation. The test passes cadence only if
the optical sequence is 90 distinct ordered frames/s for sustained intervals;
it passes ownership only if every KMS/optical event maps to exactly one R/G1/G2
tag. Report dropped/duplicate frames, queue depth, frame age, added input-to-
display latency, APU power, temperature, and memory. Repeat with the limiter
disabled and with a deliberate 30 FPS cap as a negative control. Run 10-minute
dynamic-quality and 30-minute stability/power sessions.

The current bridge is app-side and same-device: it submits capture, TE output
signals, and ordered FIFO WSI handoffs without a host pixel read. It now
requests two extra swapchain images, refuses non-FIFO mode, leaves disabled or
incapable devices' swapchains unchanged, and avoids external-FD requirements in
the same-device path. These checks reduce known failure modes; the current
`vkQueuePresentKHR` instrumentation still measures API handoffs, not KMS or
optical presentation. The shared-FD path has an explicit bidirectional startup
self-test but remains untested because llvmpipe does not support external
semaphore-FD import. No asynchronous present worker or Deck deadline profile
has been accepted yet.

The opt-in `GFG_NATIVE_PRESENT_TRACE=1` monitor now polls Vulkan present-wait
with zero timeout and a 1 ms sleep between polls. It shares a per-swapchain
host mutex with the layer's acquire, QueuePresent and application present-wait wrappers,
because the Vulkan registry marks the swapchain externally synchronized for
`vkWaitForPresentKHR`. This makes its waits valid and bounds monitor lock holds
to the duration of a nonblocking driver call. It still observes Vulkan
present-wait completion only, not KMS scanout or optical display. Lavapipe in
this executor does not expose KHR present-wait, so the active monitor path is
not exercised by the current software runtime; the contract test guards the
lock/polling invariant, and Deck validation remains required.

## Nested experiment feasibility

Meson 1.12.1 and Ninja 1.13.2 were installed under `/tmp` only. A read-only
nested configure attempt used
`meson setup /tmp/gfg-gamescope-build /workspace/gfg-research-sources/gamescope
--wrap-mode=nodownload --buildtype=debugoptimized`; it stopped at missing X11
development metadata (`Dependency "x11" not found`). The same environment has
no complete Wayland, PipeWire, libdrm, and Gamescope dependency set; no
Gamescope build or nested frame-timing experiment is claimed. The captured
configure output is in [`gamescope-meson-configure-20261016.txt`](evidence/gamescope-meson-configure-20261016.txt).

## Actionable decisions

1. Keep same-device timeline/queue ordering as the default; do not restore the
   host-signalled relay that triggered present synchronization VUID 03268.
2. Keep FIFO as the generation contract. Verify the GFG-versus-Gamescope WSI
   layer order on SteamOS; if GFG sees MAILBOX or unsupported present-chain
   payloads, pass through and log the reason.
3. Disable a Gamescope FPS cap below 90 for FG runs. Treat per-window FPS limit
   and output refresh as separate measured variables.
4. Join application present IDs to Gamescope commit IDs and actual
   `page_flip_handler` framebuffer events before reporting compositor cadence.
   Add optical capture before claiming physical OLED presentation.
5. Keep tile/LMC pixel-distinctness tests and software performance numbers
   separate from Deck quality, latency, power, 10-minute dynamic-quality, and
   30-minute stability acceptance.

## Latest software checkpoint

The latest source build passed the 21 `test_native*.py` tests, and the core
passed 168 checks in both CPU-reference and GPU-LMC modes. On llvmpipe under
Xvfb, the packaged default GPU-LMC path passed both Vulkan layer orders with 24
real presents, 46 synthetic handoffs, zero skips/errors, five distinct ordered
pixel checks, and zero validation errors. Disabled timeline and MAILBOX stayed
pass-through in both layer orders; unsupported shared-FD and the removed relay
also stayed pass-through with zero validation errors. The local ZIP passed
checksums, source-content checks, install, `--off`, installed GPU runtime,
diagnostic archive/report, rollback and uninstall. Its manifest records a
dirty worktree, so this local ZIP is not the publishable CI artifact. Full raw
logs are in [`software-checkpoint-20261016.txt`](evidence/software-checkpoint-20261016.txt).

llvmpipe reports no `VK_KHR_present_wait` capability, so present-wait mode
remained disabled in these WSI runs. The host-sync regression protects the
zero-timeout polling design, but live monitor behavior on Deck is untested.
The configured Actions workflow now attaches its clean-commit package,
checksums, build environment and software logs for seven days after the branch
push; its result is still pending until that run completes.

**Status: source-path inference is bounded; SteamOS package match, layer order,
90 Hz physical output, OLED optical distinction, latency, power, and game
quality are all UNVERIFIED.**

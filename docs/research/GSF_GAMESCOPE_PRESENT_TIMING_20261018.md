# GSF research checkpoint: Gamescope 2026 present timing and actual frame-delivery proof

**Date:** 2026-10-18. **Type:** source-level audit / actionable feasibility plan. **Status:** NOT tested on Deck; no runtime implementation or game acceptance.
**Source pins:** GSF `main` `1f58ba8e198ef82fc4781ea9242da594b8abad44`; Valve Gamescope `master` `74a06ff517ed9fc1307227406bd53ae847be3a91` (as inspected). Gamescope's first present-timing series landed **2026-10-10**, with related WSI fixes through 2026-10-16. This postdates many previous GSF Gamescope audits and **may not be in the Gamescope binary shipped on a given SteamOS installation**.

## 1. Major new finding: Gamescope has a native VK_EXT_present_timing path

The code in [FROG WSI layer](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/layer/VkLayer_FROG_gamescope_wsi.cpp) supports `VK_EXT_present_timing`, notably:
- `presentTimingSupported` at approximately lines 821–837 requires the Gamescope **swapchain factory protocol v2** plus driver-advertised `VK_KHR_present_id2` and `VK_KHR_calibrated_timestamps`.
- Advertises `VK_EXT_present_timing` at approximately line 1347, conditional on that support.
- `GetPhysicalDeviceSurfaceCapabilities2KHR` around 1250 exposes timing support **only** for X11/Xwayland bypass surfaces (`!isWayland()`, `canBypassXWayland()`), in addition to the device capability. Device extension present is **not** equivalent to the actual target surface supporting it.
- Surface stages are `VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT`, `VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT`, `VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT`.
- At swapchain creation (around 1549/1689), the layer removes `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` before forwarding to the driver's swapchain, while creating its own per-swapchain Gamescope timing queue on the upper interface. This shows **the layer, not necessarily the lower ICD**, owns application-visible timing.
- At present (around 1940–2004), it creates a serial and sends a timing request over its custom Wayland protocol; `vkGetPastPresentationTimingEXT` exposes asynchronous results. `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT` is a real, explicitly handled error when result slots are exhausted; a caller must manage queue size and drain results.
- [Protocol v2](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/protocol/gamescope-swapchain.xml) requires opt-in `create_swapchain_with_timing`, provides `set_present_timing` and `present_timing`, and **reports zero timing values for discarded commits**.

**Why GSF should care:** this is a potential **compositor-owned scheduling and drop-detection instrument**, especially for a 45 real → 90 OLED proof. It must be measured before altering the WSI pipeline.

## 2. Critical limitation: FIRST_PIXEL_OUT is predicted, NOT KMS measured

The Gamescope protocol explicitly says *"Pixel out is the compositor's predicted scanout."* See the `present_timing` event description at approximately lines 234–246. The [steamcompmgr implementation](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/src/steamcompmgr.cpp) passes `lastCommit->predicted_present_time` into the timing report in `handle_presented_for_window` around lines 8219–8224.

A separate [DRMBackend::page_flip_handler](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/src/Backends/DRMBackend.cpp) near lines 770–810 gets **actual kernel flip event timestamps** and marks the vblank, but no current GSF mechanism correlates a given generated G1/G2 with the specific DRM framebuffer ID/page flip. The DRM source even contains a TODO about matching a queued framebuffer to in-flight events when >1 in flight.

This matches a warning already present in [GSF's original SteamOS audit](../fg3/STEAMOS_DEEP_AUDIT.md): older GOOGLE timing and commit feedback never proved optical scanout. The new EXT can provide richer compositor feedback and explicit drops, but **must not be logged as observed physical OLED FPS**.

Use these distinct classifications:
1. `wsi_accepted`: `vkQueuePresentKHR` accepted/queued, no display claim.
2. `compositor_presented_predicted` / `compositor_discarded`: correlated EXT timing result (nonzero vs all-zero timing), still not hardware observation.
3. `kms_flip_observed`: Gamescope KMS event with explicit image/frame association, only if instrumented under justified privileges.
4. `optical_distinct_displayed`: actual monitored OLED content, only from a verified optical capture/other trustworthy end-to-end measurement.

Do **not** conflate Vulkan "FIRST_PIXEL_OUT" as a label with actual physical first pixel of the panel; Gamescope's v2 implementation is a **prediction**.

## 3. Driver maturity improved in 2026, but availability on the Deck is unresolved

- [Mesa 26.1.0 notes](https://docs.mesa3d.org/relnotes/26.1.0.html) explicitly list **RADV enabling `VK_EXT_present_timing`**, along with Vulkan WSI implementation.
- [Mesa 26.2.0 notes](https://kusma.pages.freedesktop.org/mesa/relnotes/26.2.0.html) include **X11/Xwayland `VK_EXT_present_timing`** support and additional WSI timing fixes.
- [Khronos EXT specification](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_present_timing.html) requires `VK_KHR_present_id2`, `VK_KHR_calibrated_timestamps`, `VK_KHR_get_surface_capabilities2` and `VK_KHR_swapchain`. The [Khronos official sample](https://github.khronos.org/Vulkan-Site/samples/latest/samples/api/swapchain_present_timing/README.html) documents querying per-surface stages, swapchain timing properties, bounded results queue, and the need to handle VRR or unknown refresh intervals.
- None of these facts establish that a **particular SteamOS image** ships a new-enough Gamescope layer, compositor factory protocol v2 or Mesa version. Actual Deck Vulkan enumeration, WSI surface-capability query, Gamescope binary/version and `VK_LOADER_DEBUG` layer order are required.

**GO/NO-GO probe on Steam Deck:** Query version of Gamescope and Mesa/RADV; check `VK_EXT_present_timing` and dependency enumeration; `vkGetPhysicalDeviceFeatures2` for `VkPhysicalDevicePresentTimingFeaturesEXT`; `vkGetPhysicalDeviceSurfaceCapabilities2KHR` with `VkPresentTimingSurfaceCapabilitiesEXT` for the **actual game surface**, not a dummy desktop surface. Confirm X11 bypass and protocol v2 are live. Fall back to existing optional GOOGLE display timing or ordinary WSI when unavailable; never assume `vkGetDeviceProcAddr` alone implies an enabled extension.

## 4. GSF code comparison: EXT metadata partially recognized, timing service NOT implemented

Inspected actual [GSF native code](https://github.com/wavessevaw/GSF/tree/1f58ba8e198ef82fc4781ea9242da594b8abad44/engine/gfg-native/):

- `src/layer.c` around lines 340–405 queries only `VK_GOOGLE_display_timing` for its own pacing and can enable `VK_KHR_present_id/_wait`. It does **not** enable/query the full EXT timing features, surface capabilities, result ring or optional `VkPhysicalDevicePresentTimingFeaturesEXT`.
- `src/gpu_bridge.c` around 220–253 and 1820–1832 schedules via **host CLOCK_MONOTONIC sleep + optional GOOGLE desiredPresentTime**; results are labelled with `physical_displayed:null`, correctly avoiding a false hardware claim.
- `src/present_chain.h` around 95–105 already recognizes and forwards `VkPresentTimingsInfoEXT` on a **real application present**, but rejects relative timing. `src/output_worker.c` deep copies the `VkPresentTimingInfoEXT` record for async use and refuses unfamiliar nested `pNext`. These are **compatibility/fail-open safeguards**, not support for instrumenting synthetics.
- **New compatibility gate to track:** `src/layer.c::native_ineligible_reason` around 1080–1090 allows only `VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR` and `VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR` in create flags. A game that uses the new legitimate `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT` will be marked ineligible for frame generation. That currently means **safe real-only fallback**, not a crash, but it should become an explicit report reason and test case.
- **Another compatibility gate:** `gfg_pc_prepare` rejects `VkPresentTimingsInfoEXT` when `pTimingInfos == NULL`. Mesa's present-timing compliance notes confirm that NULL is a permitted no-op. It is safest to preserve the original game present untouched rather than force generation, but add a test and a clear, non-misleading fallback status.
- The generated intermediates currently receive optional `VK_GOOGLE_display_timing` targets but have no layer-owned `VK_EXT_present_timing` source/synthetic ID mapping. It would be incorrect to attach synthetic presents to **application-owned present IDs**. Create an opt-in, separately versioned internal measurement namespace only when the extension/feature/flags/WSI support path is complete.
- `src/layer.c` with `VK_INSTANCE_LAYERS`-string-only packed10 Gamescope guard remains a previously identified detection-gap hypothesis. Do not bypass safety gates to enable HDR tests.

### Design caution: adding present timing is NOT one environment variable

The device extension **must be enabled** with compatible feature structs, the game-facing swapchain must be created with `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT`, and the target surface must advertise support. Taking over a game's `VkSwapchainCreateInfoKHR` and modifying `pNext`/flags can break compatibility, particularly with Gamescope maintenance1. This is a **new optional experimental capability** in GSF, not a default change to every game's Vulkan device.

Do not conflate `VK_EXT_present_timing` with `VK_GOOGLE_display_timing`; both have different records and activation contracts. A targeted `VK_EXT` probe and a separate observer should be implemented before adopting EXT as a pacing authority.

## 5. Separate unsolved obstacle: image supply / per-window frame limit

The new EXT **does not provide spare swapchain images** and cannot force a compositor to show two frames within one 90Hz refresh cycle. GSF #155 presently asks for +2 images for 2× and +3 for 3×. AFMF-Linux reports +5 extra for Gamescope in some tests, since 2 were insufficient, but that is **not an instruction to hardcode 5 on Deck**. Respect real `maxImageCount`, memory caps, latency and loaded title. Distinguish "driver reports unbounded" from "surface cap could not be queried."

GSF currently preserves an important FIFO/MAILBOX contract: [Gamescope WSI](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/layer/VkLayer_FROG_gamescope_wsi.cpp) explicitly rewrites its **driver-facing swapchain to MAILBOX** (around lines 1577–1594) but implements application-facing FIFO via its own Wayland commit protocol. Supplying an unsupported FIFO in the lower driver-facing maintenance1 mode list must remain forbidden.

A second hidden source of 45→90 failure is **Gamescope's effective per-window limiter**. [steamcompmgr.cpp](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/src/steamcompmgr.cpp) around 7950 uses limited app refresh cycle for FIFO; if Gamescope still applies a 45 FPS limit to the game window, synthetic frames cannot reliably achieve 90Hz. Measure `VkSwapchainTimingPropertiesEXT.refreshDuration` vs `refreshInterval` **when supported**, rather than assuming panel 90Hz means window 90Hz. Remove limiter only in controlled testing or obtain a dedicated 90Hz compositor presentation mode; never silently override user power settings.

## 6. Proposed falsifiable GSF experimental sequence

**P0 A: passive Deck capabilities probe (no FG):**
- Record SteamOS build, Gamescope executable version/hash and Mesa RADV version, active Game Mode/desktop, `VK_LOADER_DEBUG=layer` selection and order, enabled/available Vulkan extensions and required features, actual X11/Wayland surface type, exact capabilities and timing flags.
- Require report fields `device_timing_supported`, `surface_timing_supported`, `bypass_xwayland`, `timing_factory_v2`, `timing_disabled_reason`, `refresh_duration_ns`, `refresh_interval_ns`, `layer_order`. Unknown remains **unknown**, never guessed.
- Leave all actual game swapchain parameters and HDR guards unchanged for the first probe.

**P0 B: isolated EXT timing observer, not generator:**
- On a tested opt-in application with supported surface, request/collect per-real-present `VkPastPresentationTimingEXT` entries and count separately completed, discarded (zero stages) and missing/late reports. Test full results queue, swapchain recreate, mixed present batches, unknown `pNext` and app-owned IDs. Read the Gamescope [present timing unit tests](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/tests/test_present_timing_layer.cpp) for exact negative controls and metadata slicing behavior.
- Use monotonic timestamps; record a clean origin/clock-domain mapping. **Do not add synthetics** in this first observer to avoid corrupting the measurement.
- If a timing queue fills, record and disable the optional diagnostic rather than breaking game progression.

**P0 C: correlate existing GSF synthetic ID with Gamescope results:**
- In a controlled synthetic moving-square application, use unique visible source ID and distinct colored/spatial markers for `REAL`, `SYNTH 1/2`, `SYNTH 1/3`, `SYNTH 2/3`.
- Log per pair: app real source timestamp, GPU LMC completion, eligible spare WSI acquire, WSI submit return, layer/internal present identifier, Gamescope timing serial, compositor reported/discarded and (if available) KMS commit/flip association.
- First prove ordered 45→90 **2× SDR** on full Gamescope chain, and verify extra images alone do not simply increase queue latency.
- **Do not claim 90 physically visible frames** from EXT timestamps: optical evidence or an independently trusted frame-to-CRTC path is still required.

**P1 D: compare scheduling strategies A/B** (same game, 90Hz, same screen):
1. Current `clock_nanosleep` + `VK_GOOGLE_display_timing`.
2. Optional EXT absolute/relative scheduling only when supported (and only when safe to own IDs/flags).
3. Host bounded-output pacing with compositor event-feedback-driven deadline/drop logic.
Measure real 1% lows, input-to-photon latency, missed real frames, compositor discarded synthetics, extra VRAM, queue depth, GPU time and power. Treat no gain as permission to keep simpler strategy.

**P1 E: fallback matrix**
- No EXT support; app-owned IDs; native Wayland; no bypass XWayland; compositor protocol v1; FPS cap below 90; VRR; missing spare image; unsupported swapchain flags including new timing flag; invalid/unknown pNext; application presents two swapchains; GPU submit after waits consumed; suspend/recreate; HDR packed10. All require safe real-only behavior without regression.

## 7. Decisions and immediate agent tasks

| Priority | Task | Acceptance |
|---|---|---|
| P0 | Probe newer Gamescope 2026-10-10 timing support on **actual** Steam Deck build | Exact device+surface conditions reported, unknowns preserved |
| P0 | Add GSF new timing-flag and NULL/no-op present pNext **compatibility tests**, without suppressing old checks | App real output intact, no silent metadata rewriting |
| P0 | Keep investigating inherited Native FG3 lavapipe failure | Verified pixels, classified skips, zero synchronization errors |
| P1 | Implement an isolated EXT observer and bounded result queue | Correct present ID/serial mapping, explicit reported/discarded, no new WSI failures |
| P1 | Instrument per-window effective refresh/limiter | Demonstrate app window permits 90Hz, not just panel |
| P1 | A/B source+syn timing strategies with real image availability | Measured quality and latency improvement on Deck, rather than FPS-only |
| P2 | Optional Gamescope/DRM frame-to-KMS association path | Correlated flip IDs and optical distinct-frame acceptance |
| P2 | HDR10 and 3× | Only after SDR 2× hardware acceptance |

**Excluded from this research change:** no global manifest injection, no Gamescope binary modification, no mandatory Mesa upgrade, no changes to GFG Governor or its stable releases, no copied third-party code, no fabricated OLED measurements.

## References and exact upstream chronology

- Gamescope `master` pinned commit [74a06ff5](https://github.com/ValveSoftware/gamescope/tree/74a06ff517ed9fc1307227406bd53ae847be3a91) (source snapshot 2026-10-18).
- Gamescope protocol `VK_EXT_present_timing` introduced [2026-10-10](https://github.com/ValveSoftware/gamescope/commit/04711d2370d); [layer implementation](https://github.com/ValveSoftware/gamescope/commit/98600b3976a); [source fixes](https://github.com/ValveSoftware/gamescope/commit/4d9bd7c79d3c24087e56e7c04fd5af9029a09569) around 2026-10-16.
- Khronos `VK_EXT_present_timing`: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_present_timing.html
- Khronos timing sample: https://github.khronos.org/Vulkan-Site/samples/latest/samples/api/swapchain_present_timing/README.html
- Khronos semaphore lifecycle: https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html
- Mesa RADV extension activation: https://docs.mesa3d.org/relnotes/26.1.0.html
- Mesa X11/Xwayland support: https://kusma.pages.freedesktop.org/mesa/relnotes/26.2.0.html
- Original GSF SteamOS audit: https://github.com/wavessevaw/GSF/blob/main/docs/fg3/STEAMOS_DEEP_AUDIT.md
- Experimental GSF real WSI layer: https://github.com/wavessevaw/GSF/blob/main/engine/gfg-native/src/layer.c
- AFMF Gamescope spare image note: https://afmf-linux.digitalexperiments.dev/faq/

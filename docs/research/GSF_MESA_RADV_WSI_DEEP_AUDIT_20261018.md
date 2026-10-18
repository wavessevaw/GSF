# Mesa / RADV / Vulkan WSI deep audit for independent GSF

**Research date:** 2026-10-18. **Status:** source- and documentation-level audit, not a Deck hardware test or an implementation. **GSF target:** independent GPU Synthetic Frames 2× SDR on Steam Deck, then 3×/HDR. Stable GFG Extreme is out of scope.

## 0. Evidence pinning, not assumptions

- Mesa driver docs: https://docs.mesa3d.org/drivers/radv.html
- Mesa environment diagnostics: https://docs.mesa3d.org/envvars.html
- Mesa 26.1.0 (2026-05-06): https://docs.mesa3d.org/relnotes/26.1.0.html
- Mesa 26.2.0 (2026-08-05): https://docs.mesa3d.org/relnotes/26.2.0.html
- Mesa 26.2.4 (2026-10-01): https://docs.mesa3d.org/relnotes/26.2.4.html
- Source snapshot of Mesa Vulkan common WSI: https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/ecf8f8d26ffd5d9fdfbf67346d58b254aaf882fd/src/vulkan/wsi/wsi_common.c
- Source snapshot of Wayland WSI: https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/7215c4ef5fba349dbd5debb6c8fd336f1e3fef48/src/vulkan/wsi/wsi_common_wayland.c
- Valve Gamescope new `VK_EXT_present_timing` implementation as of 2026-10-18: https://github.com/ValveSoftware/gamescope/tree/74a06ff517ed9fc1307227406bd53ae847be3a91
- Prior GSF Gamescope audit: [GSF_GAMESCOPE_PRESENT_TIMING_20261018.md](GSF_GAMESCOPE_PRESENT_TIMING_20261018.md).
- GSF Native: `engine/gfg-native/src/layer.c`, `gpu_bridge.c`, `output_worker.c`, `present_chain.h`.
- GSF Temporal: `engine/gfg-temporal/src/te_engine.c`, `te_lmc.inc`, `shaders/lmc/*.comp`.
- Inherited CI red, reproducible and quantified: [GSF PR #4](https://github.com/wavessevaw/GSF/pull/4) and [run 39101728491](https://github.com/wavessevaw/GSF/actions/runs/39101728491).

**Important provenance distinction:** upstream Mesa current documentation/release notes and mirrored source snapshots are NOT a measurement of the Mesa/RADV/Gamescope versions installed on the user's SteamOS. Obtain the exact package build, driver ID, layer list and target surface first.

## 1. Actual boundaries of the Steam Deck graphics stack

```text
Game / Proton / DXVK (produces REAL VkImages)
   -> GSF user-space Vulkan implicit/explicit layer (capture, TE/LMC, G1/G2 output)
   -> Gamescope FROG WSI layer (application FIFO / protocol, optional EXT timing)
   -> Mesa RADV Vulkan ICD: libvulkan_radeon.so
       - SPIR-V -> NIR -> ACO -> AMD ISA
       - Vulkan command queues / vkQueueSubmit / GPU resources
       - common WSI surfaces (X11, Wayland, direct display), driver-facing swapchain
   -> libdrm / Linux amdgpu and DRM-KMS
   -> Gamescope compositor / actual CRTC page flips
   -> Steam Deck OLED
```

**Caution:** physical display composition and Mesa WSI can use alternate orders/shortcuts; the diagram is logical, not an asserted device-specific call order. A Vulkan layer placed **below** Gamescope FROG sees driver-facing MAILBOX; a layer **above** can see game-facing FIFO. Mesa is not Gamescope and cannot by itself guarantee Gamescope FIFO scheduling or scanout.

RADV is the normal Vulkan driver on Steam Deck. The APU uses RDNA2 (GFX10.3 family); generic RADV/GFX12 or RDNA4 performance metrics are not predictive. Lavapipe in GitHub CI is a **CPU** Vulkan implementation, not an emulator of Deck RDNA2 performance.

## 2. Vulkan WSI specifics GSF must honor

Mesa `src/vulkan/wsi/wsi_common.c`:
- Creates a platform-specific swapchain and may use **no blit, buffer blit or image blit** depending on WSI backend and actual allocated image type; therefore, a Vulkan `VkImage` that is successfully presented need not correspond directly to the eventual framebuffer shown on the panel.
- Determines Vulkan surface capabilities by backend; `minImageCount` and `maxImageCount` must come from a successful surface query. Vulkan's **reported** `maxImageCount == 0` means unbounded, but zero from a missing/failed query is **unknown**, not unbounded. Current GSF Direct WSI conflates the cases in its eligibility test and depends on driver-create failure to fall back. Distinguish explicitly and preserve real-only mode when unknown.
- Allocates additional resource structures for EXT present timing: device extensions, command pools, query pools and bounded timing queues may be involved. They carry CPU/GPU overhead; enable opt-in only after support is verified.
- `vkQueuePresentKHR` success is WSI acceptance; neither Mesa's queue dispatch nor its timestamp callback establishes optical pixel identity without a correlated compositor/DRM/optical observation.
- Present wait semaphore lifetime extends until a subsequent **reacquire of the same image** and completed acquire synchronization (or an explicitly supported completion mechanism). Do not resignal/reuse immediately after submitting work or after a host `vkQueuePresentKHR` return. The official Vulkan Guide documents this pitfall: https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html
- Mesa has `MESA_VK_WSI_PRESENT_MODE` overrides, which can force `fifo`, `relaxed`, `mailbox`, `immediate`. **Do not use this to 'fix' Gamescope**: forcing driver FIFO against Gamescope maintenance1 MAILBOX-only `pNext` recreates exactly the incompatibility GSF previously discovered.

### Mesa timing storage has two genuinely different semantics

In `wsi_common_allocate_timing_request` around the mirrored `wsi_common.c` lines 1720–1740:
- `VK_EXT_present_timing`: if its bounded completed-record queue is full, return `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`.
- `VK_GOOGLE_display_timing`: the older fixed-size FIFO may discard the **oldest stored timing record** to make room.
- **Consequence for GSF:** a missing old GOOGLE timing entry is **not proof of a dropped frame**; it may be **dropped telemetry**. Equally, EXT queue-full is a correctness-level observer condition, not automatic permission to retry the game's present after consuming wait semaphores.

Mesa 26.2.4 fixes present-timing and GOOGLE records when user-supplied output arrays are too small (Cooper Morgan changes). For a GSF timing observer: size-query results; allocate enough slots; respect `VK_INCOMPLETE`, `reportComplete` and partial-result semantics; test with small arrays and the exact deployed Mesa build. A record lost due to buggy readout **must not be counted as WSI image loss**.

### WSI backend matters

- Mesa 26.1: `VK_EXT_present_timing` introduced for RADV and common Vulkan WSI; Wayland support exists.
- Mesa 26.2: added X11/XWayland timing support and a series of Wayland, common and direct-display fixes.
- **Gamescope FROG WSI is a distinct upper Vulkan layer**, whose October 10, 2026 timing implementation requires a factory v2 and specific X11 bypass. It can report a compositor-predicted `IMAGE_FIRST_PIXEL_OUT`, **not hardware-observed OLED output**.
- Native Wayland, ordinary X11, XWayland bypass, nested Gamescope and SteamOS Gaming Mode have different surface/timing feature availability. Query actual `VkPresentTimingSurfaceCapabilitiesEXT` for the game's real surface.
- Wayland `presentation-time`/commit timing guarantees depend on compositor protocols; no universal guarantee of a physically unique frame from a present request. Maintain `compositor_discarded`, `WSI-accepted`, `KMS-observed` and `optically observed` as **separate evidence levels**.

## 3. RADV shader compiler and GSF GPU LMC: concrete source findings

Mesa's documented compiler chain is `SPIR-V -> spirv_to_nir -> NIR optimizations/lowering -> ACO -> machine ISA`. ACO is default for RADV. Shader compilation cost and workgroup occupancy depend on GPU generation, shaders, specialization constants, memory behavior and scene parameters.

The GSF source itself:
- `te_engine.c::make_pipeline` around lines 736–747 calls `vkCreateComputePipelines(..., VK_NULL_HANDLE, ...)` for ordinary Temporal Engine.
- `te_lmc.inc` around lines 481–523 does the **same** for multiple LMC compute pipelines and a **grid × pyramid-level family of specialized search pipelines**.
- GSF currently has **no application-owned `VkPipelineCache`** in those call sites. This is *not* evidence that all pipeline compilation repeats: RADV/Mesa implements shader/disk caching independently.
- GPU LMC's `te_engine.c::validate_desc` around lines 964–969 insists on `maxComputeWorkGroupInvocations >= 512`, X dimension >=512, >=16,900 bytes shared memory and compute subgroup arithmetic for relevant modes. This is a feature gate; meeting it does **not** prove suitable occupancy or performance on the Deck's 8-CU APU.
- `shaders/lmc/lmc_irls.comp` uses `layout(local_size_x=512)` and substantial `shared` arrays. Workgroup size, shared LDS, scalar/vector register pressure, wave32 vs wave64 and inactive lanes are actual performance experiments to run on RADV, not ACO tweak assumptions.
- GSF shader SPIR-V is generated/embedded at build time; pipeline specialization and Vulkan pipeline creation happen at runtime. First GPU dispatch also allocates/synchronizes resources and may trigger additional lavapipe CPU execution setup. Separate these phases rather than call every first-frame delay a 'shader compile'.

### Findings from the newly reinstated strict lavapipe CI

[GSF native GPU CI run 39101728491](https://github.com/wavessevaw/GSF/actions/runs/39101728491):
- Native-first: 8 real, 12 synthetic, 2 skipped (history), 2 verified+distinct pairs, 2 stale worker jobs, zero Vulkan errors. First observed LMC pair ready at ~12.8 s; subsequent ~19.7–19.9 ms.
- Validation-first: 8 real, 10 synthetic, 4 skipped (history), 1 verified+distinct pair, 3 stale jobs, zero Vulkan errors. First pair ready at ~20.85 s; subsequent ~67–74 ms.
- First motion model stage took ~6.30s / ~9.56s on software Vulkan, versus ~6–14 ms on later pairs; these stats **do not isolate** shader JIT vs CPU scheduling vs actual slow work. No Deck perf conclusion follows.
- The Output Worker marks jobs stale after 2 real frame periods (~66.667ms in this test), causing expected protective history drops under very late readiness. This is a **mixed startup/correctness test**; do not weaken assertions just to make CI green.
- A valid split is deterministic startup/fault recovery with classified skip allowances and a separate, **explicitly prewarmed**, strict non-skipped/pixel-verified steady-state suite.

## 4. Mesa already supplies powerful profiling/diagnostic tools

Always use **process-scoped** diagnostic environments in a test launcher, never globally on SteamOS:

| Variable / API | Intended test | Caution |
| --- | --- | --- |
| `MESA_SHADER_CACHE_SHOW_STATS=true` | print cache hit/miss statistics at process end | Compiler cache misses are **not** output-frame drops |
| `MESA_SHADER_CACHE_DIR=<isolated cache path>` | compare cold first run with warm second run | Do not wipe user's general shader cache |
| `MESA_SHADER_CACHE_DISABLE=true` | negative cold-cache control | Experimental only; may increase game stutter |
| `RADV_DEBUG=psocachestats` | RADV PSO cache hit/miss analysis | Availability depends on deployed RADV build |
| `RADV_DEBUG=shaderstats` | shader resource/ISA statistics | Heavy logging can alter timings |
| `RADV_DEBUG=hang` | GPU hang dump on controlled failure tests | May write very large sensitive device logs |
| `MESA_VK_TRACE=rgp` and optionally `MESA_VK_TRACE_PER_SUBMIT=1` | Radeon GPU Profiler capture for compute and synchronization | Trace overhead, storage and tooling; separate benchmark runs |
| Vulkan `vkCmdWriteTimestamp` / `vkGetQueryPoolResults` | per-stage GPU elapsed timing | Check timestamp valid bits and device timestampPeriod; no host GPU-wait on present path |
| `RADV_PERFTEST=cswave32` | diagnostic A/B compute wave32 versus default | Not a general improvement; do not ship as required env |
| `RADV_DEBUG=nodcc` or related DCC controls | investigate format/compression/path issue | Can degrade performance and memory bandwidth; not a global workaround |

Actual official Mesa docs: https://docs.mesa3d.org/envvars.html and https://docs.mesa3d.org/drivers/radv.html.

### Profiling contract

Capture before enabling generation:
1. `t_spirv_shader_module`, `t_create_pipeline` for each pipeline and specialization, `t_gpu_first_submit`, `t_gpu_first_completion`, `t_output_1_ready`, `t_output_2_ready`.
2. RADV/CPU shader cache hit/miss (or known unavailable). Separate **cold new process** from **warm identical process and cache**.
3. GPU timestamp breakdown of luma pyramid, motion searches, IRLS/model, composite and copy, with a stable source scene, plus missing/overflow query status.
4. Timestamp host wait separately; never confuse CPU elapsed inside `vkQueuePresentKHR` with total GPU time or optical scanout.
5. Stage-by-stage median/p95/p99, warmup count, and sample count. Include total APU power, game real FPS and input lag for Deck hardware.

Validate sources, overlays and visual correctness. If the benchmark is CPU lavapipe, label **CPU Vulkan** and never extrapolate to RADV.

## 5. New RADV queue capability: VK_KHR_internally_synchronized_queues

Mesa 26.1 release notes list `VK_KHR_internally_synchronized_queues` for RADV. Vulkan explicitly requires applications to opt in at **device-creation** time with a feature and `VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR`. This allows simultaneous Vulkan queue calls without the application supplying its own external queue mutex **on the queues created with this flag**.

For GSF, this is worth evaluating because Vulkan queue ownership crosses game/GSF threads. However:
- GSF is an injected layer, **not the only app module** and cannot assume the game opted in.
- Setting an unknown new queue flag or extension in a borrowed device without checking support and preserving the game's `VkDeviceCreateInfo` can break launch and dispatch semantics.
- The extension does not free us from **swapchain external synchronization**, image acquires, present wait semaphore lifetimes or correct queue ordering.
- It can move host locking into the driver and may or may not reduce critical-section duration. Benchmark **only** in an isolated test app with explicitly enabled features/flags and driver support. Do not replace the existing GSF output-Wsi mutex by default.
- Relevant spec: https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_internally_synchronized_queues.html

## 6. Key architectural questions resolved vs still open

| Question | Evidence-based answer |
| --- | --- |
| Should GSF fork Mesa or modify RADV immediately? | **No.** First get native layer + Gamescope output proven and profile bottlenecks. Mesa is complex, version-coupled and is a separate component. |
| Can the current GSF output automatically become 90 visible FPS by setting Mesa's present mode? | **No.** Gamescope's application-FIFO and driver-MAILBOX protocol remains a distinct scheduling stage. |
| Does VK_EXT_present_timing report displayable frames? | Conditionally reports compositor/WSI timing and discarded requests. The Gamescope PIXEL_OUT field is **predicted**, not an optical reading. |
| Can Mesa shader cache solve the 12–21s software startup stalls? | **Unknown.** Need explicit pipeline creation timings and first-submit GPU timestamps. ACO/RADV and lavapipe are separate compilers. |
| Does RADV 26.2 guarantee extension support on Steam Deck? | **No.** Check deployed SteamOS and actual game surface/feature enablement. |
| Can we run 2×/3× with an arbitrary number of extra swapchain images? | **No.** Honor advertised image caps, actual allocation and age/latency budgets. |
| Are compute wave32 and GPU pipeline cache clearly faster? | **No.** A/B only after instrumenting LDS, registers and cache hit/miss. |
| Should GSF adopt Mesa's internal queue synchronization immediately? | **No.** Opt-in at device creation and not a substitute for correct swapchain/semaphore lifetime. |

## 7. Prioritized concrete action plan for Codex / independent GSF

**P0 / Capability probe:** implement a passive Deck report (no swapchain changes, no SteamOS root modifications) of `VkPhysicalDeviceDriverProperties`, RADV driver version and `pipelineCacheUUID`, Vulkan features/extension flags, compute shared-mem limits, subgroup size/supported ops, queue families/count/flags, surface caps/modes/maintenance1, Gamescope layer order, `VK_EXT_present_timing` device+actual-surface support, and display timing refresh. Read-only, fallback-safe; unsupported = "unknown" rather than "false success".

**P0 / Native FG3 cold/warm instrumentation:** time `vkCreateComputePipelines` for each Temporal and LMC pipeline; time first shader submit vs first completion; GPU timestamp search/model/output. Use isolated cache dir and optional RADV stats; create strict warmed deterministic 8-frame test with real=8 synthetic=14 skipped=0 and positive distinct pixel verification **without suppressing** original red startup test. Only after this split analyze true algorithmic regression.

**P0 / WSI proof:** add explicit internal statuses `acquire_success`, `wait_consumed`, `compute_ready`, `wsi_accepted`, `compositor_reported`, `compositor_discarded`, `KMS_observed`, `optically_visible`, each requiring appropriate evidence. Preserve real-frame metadata and unknown pNext passthrough.

**P1 / Compile/cache experiment:** after base tests, optional `VkPipelineCache` seeded from supported driver metadata (cache header contains vendor/device/UUID; invalidate when shader/source/driver changes). Compare ACO disk cache alone vs app cache **by instrumented cold/warm total startup**, not solely GPU FPS. Keep default behavior if gains are absent.

**P1 / RDNA2 occupancy tests:** 512-thread IRLS shader, two search specialization variants per pyramid level, possible wave32 A/B, shared-memory pressure, intermediate R16F vs R32F, direct sampling vs copy. Use actual Steam Deck GPU timestamps and power, not lavapipe time.

**P1 / Timing integrity:** tests against Mesa 26.1, 26.2 and 26.2.4 where feasible, including small timing result array, EXT queue-full, GOOGLE overwriting oldest, `VK_INCOMPLETE`, partial completion, missing timestamp, VRR and FPS cap. Never interpret unavailable feedback as dropped display.

**P2 / Deep Mesa integration (only if evidence demands):** if native layer fails despite correct chain, use source-debug builds to trace `src/vulkan/wsi/{wsi_common.c,wsi_common_wayland.c,wsi_common_x11.c}`, RADV queue submission and Gamescope's FROG/DRM present events. Propose upstream Mesa issue or patch only with minimal reproduction and exact version. Avoid bundling private Mesa changes into a Decky plugin.

## 8. Minimum acceptance matrix

| Platform | Real 2× pixel checks | Presentation semantics | Performance claim |
| --- | --- | --- | --- |
| lavapipe / Xvfb | Deterministic, warm, positive verified pixels | Mock/real X11 WSI, no physical Deck | **None** |
| RADV / desktop X11 | Positive synthetic and original checks | Surface caps, Vulkan layers, compositor tracing | Measured for tested AMD GPU only |
| RADV / native Wayland | Positive synthetic and original checks | Native Wayland timing protocol and external sync | Tested platform only |
| Steam Deck / nested Gamescope | Positive G1/G2/real and sequence | FROG layer order, app vs driver mode | Does not prove Gaming Mode/OLED |
| Steam Deck OLED / SteamOS Gaming Mode | Optical or trusted end-to-end distinct frame ID + KMS correlation, measured input latency and GPU cost | 45 real → 90 visible at 90 Hz without dropping real frames | **Only here** claim Deck GSF 2× hardware acceptance |

**Safety gate:** no modification of global Mesa ICD selection, Vulkan manifests, user shader cache or system packages during research. No GFG Extreme stable changes, no external Lossless Scaling dependency and no unlicensed source transplant.

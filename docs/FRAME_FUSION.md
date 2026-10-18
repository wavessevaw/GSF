# Frame Fusion

Frame Fusion is a native renderer optimization available only in Autopilot with two paths. Fusion Core shares compatible images with the open MAKO spatial scaler and removes one reconstruction-input blit. Native Fusion preserves the source layout at equal resolution and replaces the source conversion blit with a same-format copy. The original frame generator, two historical source images, output copies and renderer scheduling remain owned by their existing components.

## Use

Install stable 2.1.0 and its bundled engine, start the game through the managed launcher and choose Autopilot. The Autopilot card exposes the default-on Fusion switch. Start/stop and mode changes project the GFG-owned `gfg_frame_fusion` live field into the runtime overlay; Saved is never changed by these projections. The launch marker only permits provisioning. Both the applied and requested live scope must permit the shortcut; leaving Autopilot immediately closes raw native transfers and direct scaler inputs, even if private-group replacement fails. Existing valid histories use the ordinary blit until replacement commits.

The canonical private-resource transition rebuilds the input group when scope changes, preserves image/FD ownership and uses the existing retirement barrier. Scope is part of the resource key and pending request identity. Returning to Autopilot reopens shortcuts only after compatible resources are prepared. A game started with Fusion provisioning off needs relaunch to enable it. Other modes never execute these shortcuts.

Logs retain the actual paths, counters, launch setting, mapped library hashes and export segment index. Repeated counter publications are never added together. Hash identity and submitted work do not prove GPU completion or a benefit. Native Fusion was observed in the 2026-10-12 19:43 field recording; an uncontrolled second recording does not establish its effect on watts, FPS or latency.

For a useful device comparison, record fresh on/off launches in the same scene with matching resolution, scaling, FG ratio and TDP. Disable Boost/Act for the comparison so adaptive cadence and power do not confound Fusion. Compare delivered frames, misses, frame times, discharge and visible quality, then repeat. The export now preserves segments across a launch rotation and flags missing/truncated segments explicitly.

## Native implementation

### Native-resolution input (stable 2.1.0)

Some games ignore a requested low render resolution and submit 1280×800 into a 1280×800 presentation surface. These games cannot exercise Core's reconstruction shortcut. Native Fusion instead retains BGRA8 bytes in the two private source images and imports matching sampled BGRA8 images into the backend device. Vulkan sampling produces the same logical RGBA values; model and output encoding remain SDR8/RGBA. RGBA8 also uses the raw transfer path but has no channel conversion to avoid. The native layout is explicitly separate from model colour encoding.

Capability negotiation checks transfer/sampling support and opaque-FD sharing on both devices. If an eligible native group cannot be built, the existing canonical owner tries the default group once before the normal presentation fallback. No WSI allocation is imported or held by the backend; private history copies still protect the previous frame's lifetime. This stage does not reduce the number of source transfers or history allocations.

The optional `frame_fusion` extension now has schema 2; old schema 1 recordings remain readable. Native evidence includes two prepared history inputs, `native_source_transfers`, `native_conversion_blits_avoided`, actual `native_source_format`, and an explicit fallback reason. These counts are separate from Core's `avoided_source_copies`. A prepared group is not submitted work, and submitted work is not GPU completion or proof of an energy gain. Native-resolution work can be observed with spatial scaling inactive.

### Spatial input (2.1.0)

At Deck/1080p-class extents, MAKO ordinarily reconstructs the real frame before FG. Fusion negotiates sampled-image usage only at swapchain creation when the actual surface and linear UNORM format support it. A bounded borrowed view samples the logical source rectangle directly from the larger application image. The sampler clamps that rectangle before mapping into physical texture coordinates, preventing neighboring pixels from entering reconstruction. The original image ownership, FG output, return copy, layer order and frame fences remain in place. Nonlinear sRGB and HDR views cannot use this shortcut.

Above the existing pre-FG pixel budget, the original graph generates at source resolution and reconstructs each generated frame. Fusion prebinds matching private FG outputs and samples them directly. No WSI storage usage or effect ordering is changed.

Binding replacement is transactional, capacity is bounded, recording allocates nothing, and source-read layouts are restored. Scaler/backend replacement and retirement drain the existing owners and clear borrowed bindings. The legacy private images remain available for fallback: removing one transfer is not a claim that all intermediate allocations were removed.

## Build and validation

`engine/gfg-fusion/source.json` pins upstream MAKO Renderer v4.0.0 and the reviewed patch checksum. `scripts/build_frame_fusion.py` verifies that identity, regenerates shader payloads with the owning upstream generator, builds the actual production code, runs full CTest and compares the paths on a Vulkan device. The patch does not contain licensed model payloads.

The native harness checks copy commands, alternating private sources, format/extent/usage rejection, descriptor rollback, clearing, unknown sources, RGBA/BGRA cropped inputs, application-image lifetime and FG handoff. Private generated-image comparisons require byte identity; cropped application sampling permits at most one 8-bit code value of difference from normalized-coordinate rounding. The CI device is Lavapipe and validation checks synchronization. Its graph timing is not Steam Deck performance or energy evidence.

Native-source checks compare source bytes/alpha and logical sampler output against the legacy RGBA conversion, alternate two history slots, verify the previous history remains intact, and count exactly one raw copy replacing one blit. The harness uses the production transfer primitive and Vulkan sampler on private images. It does not exercise licensed LSFG or cross-device FD transport end to end. Portable policy and existing FD-ownership tests cover eligibility, format contracts and bounded ownership; real import, model execution, replacement failures and device recovery require hardware qualification.

The release builder replaces only verified native x86_64 renderer/CLI payloads in a checksum-verified archive, preserves the original 32-bit and Flatpak paths, records source and payload identities, and validates the final Decky ZIP. The package uses the existing local `bundled_renderer` contract; Decky does not fetch a modified payload from an upstream URL.

Steam Deck LCD/OLED, RADV, real LSFG execution, Gamescope WSI presentation, long sessions, suspend/resume and whole-device energy require subsequent device evidence. Stable packaging does not qualify unmeasured hardware performance. In particular, a smaller copy count does not by itself establish an overall speedup: negotiated usage and direct reads can change driver costs.

## Further Frame Fusion

Combining compatible post-processing passes is a separate measured candidate. Shared temporal history and motion data require an explicit open executor contract. The current LSFG interface does not export those maps; the first-stage patch makes no such claim.

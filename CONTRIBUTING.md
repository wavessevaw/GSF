# Contributing to GSF

GSF is an experimental Linux / SteamOS Vulkan frame-generation project.

- Work on an issue or focused pull request based on `main`; keep research and runtime changes separately reviewable.
- Never claim physical Steam Deck scanout based only on a successful Vulkan `vkQueuePresentKHR` return, synthetic counter or lavapipe test.
- Native work must preserve game semaphore consumption, image lifetime, present IDs/metadata and safe real-only fallback.
- Build `engine/gfg-temporal` first, then `engine/gfg-native` with `ENABLE_TE=1`. Vulkan shaders are generated at build time, not committed as opaque binaries.
- Test software Vulkan correctness, fault injection and both loader orders; verify pixels and classify duplicates/skips.
- Document affected Vulkan extensions, exact game environment, reproducible commands, benchmark GPU + frame timings, and limitations.
- Preserve third-party license/attribution notices. Do not introduce Lossless Scaling as a runtime dependency.
- Packaging and UI changes belonging to GFG Extreme should be implemented as versioned **consumer** integrations, not bundled into GSF core.

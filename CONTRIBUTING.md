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

## Intellectual property and contributions

- By submitting a contribution, you confirm that you have the legal right to submit it under **GPL-3.0-or-later**, except clearly identified separately licensed third-party content compatible with this project. Keep the original author, copyright and license notices.
- Please include a Developer Certificate of Origin sign-off in each commit (`git commit -s`); this certifies the contribution's provenance, **not** a copyright assignment to GSF, `wavessevaw`, or another party. See [DCO](https://developercertificate.org/).
- Prefer `SPDX-License-Identifier: GPL-3.0-or-later` at the top of newly written GPL-covered source. Preserve a file's existing SPDX expression when different and investigate whether it is compatible before importing.
- Do not submit proprietary blobs, extracted commercial shaders, private game assets, or code copied from AFMF-Linux, FidelityFX or MAKO without the applicable notices and a traceable source/version. Research links alone do not mean their source is vendored.
- Do not modify or remove existing license notices, including historical GFG/MAKO notices, to claim exclusive ownership of code by repo maintainers.
- A separate trademark or official release policy does not limit the freedoms granted by the code's GPL license.

# Optional present timing arrays: synchronous and asynchronous handling

## Defect and bounded fix

Khronos permits `VkPresentTimingsInfoEXT::pTimingInfos = NULL`:
https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentTimingsInfoEXT.html
(`VUID-VkPresentTimingsInfoEXT-pTimingInfos-parameter`).

GSF's `gfg_pc_prepare()` rejected this valid no-record array, causing the
caller to drain queued output and use synchronous passthrough unnecessarily.
Simply removing that rejection is unsafe: `copy_job()` unconditionally read
`pTimingInfos[0]`. Both sites now preserve NULL without dereferencing it.
The copied timing node and its suffix remain intact. Actual timing records
still retain the same relative-time and ID-namespace restrictions, and
unknown nested timing extensions still request synchronous fallback.
This does not enable present-timing swapchain creation flags or remove the
existing real-only create policy for those swapchains.

## Local evidence

Pinned Vulkan-Headers v1.4.365, commit
`c46850864f4661461b0f6cb9922c058ffea4915e`, GCC, `-Wall -Wextra -Werror`.

- New regression against the original parser: assertion failure on the valid
  NULL array, as expected.
- New regression with the fixed parser and original worker: segmentation
  fault on NULL dereference, as expected. Reproduced only in a temporary test
  tree, with core dumps disabled.
- Fixed parser and worker: PASS for optional NULL array, immutable suffix and
  app IDs, invalid count, duplicate metadata, relative-time/namespace rejection,
  queued jobs after caller memory is protected with `PROT_NONE`, and deferred
  device errors. Existing non-NULL deep-copy coverage remains.
- Both tests pass with AddressSanitizer and UndefinedBehaviorSanitizer.
  LeakSanitizer is unavailable in this container (cannot inspect `/proc`
  thread state); sanitizer execution used `ASAN_OPTIONS=detect_leaks=0`.
- Non-TE native layer compilation and output-owner, virtual-source,
  frame-clock, present-IDs, virtual-WSI, virtual-layout, offscreen source pool
  and memory-policy contracts: PASS. License notice safeguard: PASS.

`GSF Standalone Native` now runs `output-worker` as well as `present-chain`;
previously the asynchronous worker test was absent from active workflows.
Full Temporal/TE-enabled builds, software GPU/validation matrix and output
proxy tests are delegated to the existing GitHub Actions workflows. No Deck,
Gamescope or physical scanout acceptance is inferred from these contracts.

## Reproduction

```sh
make -C engine/gfg-native VULKAN_HEADERS=/path/to/Vulkan-Headers/include present-chain output-worker
ASAN_OPTIONS=detect_leaks=0 make -C engine/gfg-native \
  BUILD=build-sanitize VULKAN_HEADERS=/path/to/Vulkan-Headers/include \
  CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Werror -fPIC -fsanitize=address,undefined -fno-omit-frame-pointer' \
  present-chain output-worker
```

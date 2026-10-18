# GSF Autonomous Vulkan Test Lab (headless software GPU)

**Status:** autonomous CI regression environment, not a virtual Steam Deck. Runs on Ubuntu 22.04 hosted GitHub Actions with Mesa **lavapipe**, Vulkan Loader, validation layers and **Xvfb/X11**. It never installs Vulkan layers globally, modifies SteamOS or requires a physical Steam Deck.

## What is verified

The isolated **TEST-ONLY** layer receives a real Vulkan test client's `vkCreateSwapchainKHR` / `vkAcquireNextImageKHR` / render / `vkQueuePresentKHR`. It copies the source image through GPU-backed Vulkan objects and hands frames to **one actual downstream X11 output swapchain**. Each scenario verifies exact existing success markers and rejects any validation-layer errors, missing expected output, nonzero exit or deadline timeout.

**Automated cases:**

1. Real-only baseline, 8 actual GPU-rendered source images.
2. GPU A/B split, 8 source + 7 distinct software-generated **intermediate split images**, with unique X11 sampled output.
3. Paced 30→60, 8+7 frames.
4. Paced 45→90, 8+7 frames.
5. Sustained paced 45→90, **120 source + 119 intermediate = 239 X11-composited unique images**.

**Important:** the full virtual WSI suite is valid only with **GSF native layer before validation** in the Vulkan dispatch chain. An independent discovery run with validation **above** the GSF private virtual handle failed with `VUID-vkGetSwapchainImagesKHR-swapchain-parameter` and exit 139 (documented in [P0 issue #14](https://github.com/wavessevaw/GSF/issues/14)). The second job runs three **real Vulkan WSI passthrough** 4-frame cycles, PLUS a new **explicit negative safety test**. It deliberately asks to create a test-only virtual swapchain with validation ABOVE GSF and requires `VK_ERROR_FEATURE_NOT_PRESENT`, an explicit `virtual_handle_returned=0` diagnostic, NO VUID and NO crash. This proof covers only the exact `VK_INSTANCE_LAYERS` order visible to our controlled test process; it does **not** establish production-wide layer-chain detection or compatibility with a private virtual handle. We do not silence errors or classify the historical crash as success. Each tested scenario creates a fresh X server; no graphics test is `continue-on-error`.

**Hard limitation:** Intermediate images are deterministic **A/B split test patterns, not GSF's optical-flow/LMC output**. X11 pixel checks and 90-target pacing do not prove 90 physical OLED scanouts. No real Deck, RADV, Gamescope embedded session, Proton game, HDR, VRR, motion quality, power or user-input latency is tested. The older [strict real Native GPU LMC regression in experimental PR #4](https://github.com/wavessevaw/GSF/pull/4) must remain a separate non-weakened GPU correctness gate until its warm/cold issues are resolved.

## Scheduled operation

[Workflow: GSF Autonomous Vulkan Test Lab](../../.github/workflows/gsf-autonomous-lab.yml).

- Scheduled **daily at 03:17 UTC** on the default GitHub branch (subject to GitHub Actions schedule delays).
- Runs on PRs changing Native layer source, source WSI test, the test lab script, or its build/workflow contract. Also supports manual `workflow_dispatch`.
- Builds the isolated test-only output-layer probe using **Khronos Vulkan-Headers tag v1.4.365** and distro Mesa/validation packages.
- 2 matrix jobs with **different explicit scopes**: `native-first` = full virtual WSI 8→15/120→239 pixel checks; `validation-first` = Vulkan Loader **real-WSI passthrough** 3×4 real frames, plus a separate safely rejected attempt to create an experimental virtual handle. General virtual WSI compatibility for the second order is an **open failure**, not solved by this lab.
- Saves the **entire stdout/stderr of each scenario**, provenance (`dpkg-query`, Linux kernel, Vulkan-Headers commit, library checksum, `vulkaninfo`), `results.json`, and `SUMMARY.md` in downloadable GitHub Actions artifacts for **21 days**, including failed runs.
- Hard wall-time limit per process and entire job. Nonzero graphics failures make the workflow red.

### Inspect results

Open GitHub → Actions → **GSF Autonomous Vulkan Test Lab** → latest run → choose a job / artifact:

```text
gsf-vulkan-lab-native-first/
  environment.txt
  vulkaninfo.txt
  results.json
  SUMMARY.md
  real-only.log
  gpu-a-b-split.log
  paced-30-to-60.log
  paced-45-to-90.log
  sustained-45-to-90.log
gsf-vulkan-lab-validation-first/
  environment.txt
  vulkaninfo.txt
  results.json
  SUMMARY.md
  real-wsi-passthrough-1.log
  real-wsi-passthrough-2.log
  real-wsi-passthrough-3.log
  outer-validation-virtual-rejected.log
```

The result's declared evidence level is `software_vulkan_x11_test_lab`. Never quote `X11 visible` as `Steam Deck OLED physically displayed`.

## Reproduce on an independent Linux x86_64 workstation

Prerequisites: `python3`, `make`, `cc`, `xvfb-run`, `libvulkan-dev`, `mesa-vulkan-drivers`, `vulkan-validationlayers`, `libxcb1-dev`, `xauth`. Use the same Vulkan-Headers v1.4.365 as GitHub Actions.

```bash
git clone --depth 1 --branch v1.4.365 \
  https://github.com/KhronosGroup/Vulkan-Headers /tmp/gsf-vkh
make -C engine/gfg-native BUILD=build-lab \
  VULKAN_HEADERS=/tmp/gsf-vkh/include \
  build-lab/loader-probe/libVkLayer_gfg_native.so \
  build-lab/loader-probe/VkLayer_gfg_native.json \
  build-lab/loader-live-output

python3 engine/gfg-native/scripts/gsf_test_lab.py \
  --profile sustained --layer-order native-first \
  --build-dir engine/gfg-native/build-lab --output gsf-lab-results
```

Run `--layer-order validation-first` separately to test **real WSI passthrough and pre-creation rejection of known unsupported virtual-handle ordering**. This remains a narrow explicit-environment fail-closed guard, **not** general virtual WSI compatibility; [issue #14](https://github.com/wavessevaw/GSF/issues/14) remains open. Local errors do not require changing Steam or Deck settings.

## What a failure means

- **Build / Vulkan ICD missing:** environment/dependency mismatch, supported by `environment.txt`.
- **Vulkan validation error:** real Vulkan object/sync/lifetime incompatibility in this test route; fix rather than suppress.
- **Missing X11 pixel marker:** cannot claim the expected software output. Inspect log + source IDs and X11 readback before any driver or shader change.
- **Hang / timeout:** suspect virtual Acquire reuse, source retire waits, output pacing or Xvfb startup; preserve original failed case.
- **Virtual WSI with validation above GSF:** a reproducible **known interoperability crash**, issue #14; not a supported test configuration. Validation-first tests must keep real WSI passthrough and report that scope. Do not present that result as a virtual-owner pass.
- **Both pass:** proves exactly the test-only Vulkan/X11 contract; does not certify Steam Deck.

## Further validation ladder

1. This software lab must remain green.
2. The separate Native GPU LMC strict warmed-pixel test must become green **without relaxing pixel or drop assertions**. Software cost on lavapipe is not RADV throughput.
3. After 1 and 2, test user-approved production-style Vulkan/Proton game compatibility on suitable Linux hardware (still not physical OLED).
4. Only later use a Deck for targeted hardware certification under [P0 hardware acceptance issue](https://github.com/wavessevaw/GSF/issues/11). The user does **not** need to test each incremental source change.

Stable GFG Extreme and Gamescope are **not** modified by this test lab.

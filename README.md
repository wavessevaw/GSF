# GSF 0.1.13 Experimental — Stability 1

Latest installable Decky ZIP: **GSF-Decky-Test-0.1.13-stability.1.zip**

Source: https://github.com/wavessevaw/GSF/commit/375ac52d513eaa9fd295f7c9ea77a9529ebf92cd

Install over the previous GSF Test package and restart the game. For the reported Assassin's Creed Origins workload, start with Buffer 2 and Balanced; cost adaptation can reduce effective quality while retaining the selected multiplier.

Changes: compatible FIFO WSI can reserve additional real images and submit future compositor-timed slots without sleeping until the previous frame. The ordinary path falls back to Buffer 0 when private compositor timing is unavailable. GPU ring doubles from 2 to 4 when buffering is active. Shared refinement is cheaper and budget refusal no longer forces an empty history-rebuild frame in the buffered path.

The full independent Output Owner remains a separate limited opt-in contract. No production compatibility or physical-FPS claim is made.

SHA256: 5d04086ed52f528c316790eaffbc3a5908d4818f4be4d312dae72bccc39d9fdf

Four Flow Lite 2.0 suites each passed 197 checks. Decky checks and 41 Python tests passed. The extracted installable ZIP passed actual software Vulkan x2/x3/x4, including the unchanged >=60/70 x2 acceptance gate. Old diagnostic Flow Lite 1.0 retains four known adverse chroma/cut failures.

The hardware report used for diagnosis is from the previous build. Post-fix Steam Deck FPS, source performance, power and latency are not yet measured. Read STABILITY_1.ru.md for findings, software cost comparison and remaining limitations.

Original preview and Launch fix ZIPs are retained. This artifact branch does not replace the stable branch.

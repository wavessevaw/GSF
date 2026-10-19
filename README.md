# GSF 0.1.13 Experimental — Launch fix 1

Installable Decky ZIP: GSF-Decky-Test-0.1.13-launchfix.1.zip

Source: https://github.com/wavessevaw/GSF/commit/8dabfa9b605272d99caa54f5198e5100303aae20

Choose a game, press «Ручной запуск · показать команду», then copy the displayed command into Steam launch options and restart the game. Existing launch options must be preserved. Manual setup does not claim Steam accepted a write. Progress and errors are now visible at the top of the panel.

SHA256: c01834f269940085034b95e96debf574ee01a8d0d2d1bdab2d4103161fdaf0a4

41 backend tests, frontend checks, shipped-panel tests and the extracted-package software Vulkan test passed. The extracted Vulkan test includes x4 from a manually prepared profile.

Steam Deck hardware, Steam CEF/controller/clipboard integration and physical display FPS remain unverified. The independent Output Owner still has the limited opt-in WSI contract described in PR #51. This experimental package does not replace a stable release.

The original preview ZIP is retained alongside the fixed ZIP.

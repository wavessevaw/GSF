# GSF Next provenance

This engine is a modified fork of afmf-linux 1.3.1 by Lain:
https://github.com/serialexperimentslainnnn/afmf-linux
Pinned commit: bc56e5bd0e3a6c328aedef664d0d36363fb58b36.

Upstream sources retain their MIT license (LICENSE). FidelityFX Optical Flow
headers/shaders retain AMD's MIT notices in shaders/fidelityfx/LICENSE.txt.
They are independent of the commercial AMD driver AFMF implementation.
GSF changes: per-output temporal phases and shared optical flow; display-slot
admission; bounded two-job queue; mandatory actual interpolation; safe submission
failure; process-local Decky launcher; Vulkan X11 test coverage.

No Lossless Scaling executables, models, DLLs or shaders are included or needed.
No endorsement or Deck compatibility qualification by AMD or upstream is implied.
The old native GSF engine is not loaded by this plugin.

# AC Origins: generator refused the swapchain (2026-10-18 UTC)

Owner archive: `GSF-Game-Latest.zip`, session
`game-582160-20261018T140954Z-8y9ydtxi`, build `df1dbe191be8df7bccbd1df4e98fbd78dcda1a46`.
The game is ACOrigins.exe, DXVK 3.1.1 (engineVersion 12587009).

The native layer loaded and logged gpu_requested=1/gpu_capable=1, but the
create gate returned candidate=0, reason=unsupported-swapchain-flags.
The original FIFO swapchain was created as a pass-through. No GPU bridge,
status records or frame telemetry were created; generated count is zero.
The display refresh notification is 11.11 ms (90 Hz). This does not establish
the game's source rate or its limiter setting. Overlay 45 FPS cannot be
explained as a confirmed 45→90 run from this archive.

The launcher exit -2 is SIGINT, not evidence of a GPU crash. No crash trace
establishes an additional renderer failure here.

[DXVK 3.1.1 presenter](https://github.com/doitsujin/dxvk/blob/v3.1.1/src/dxvk/dxvk_presenter.cpp)
sets MUTABLE_FORMAT for an UNORM/SRGB view list, optionally combined with
PRESENT_ID_2/PRESENT_WAIT_2. GSF already allowed the latter two, but rejected
MUTABLE_FORMAT. The old gate did not print numeric flags, so this identifies
a matching implementation gap rather than proving the exact bit in the Deck
binary's request.

The fix preserves the application's flag/list, permits only same-family
RGBA8/BGRA8 UNORM/SRGB aliases with a list containing the base format, and
keeps protected, unknown and unverified format families rejected. The GPU
transport copies the base image and does not reinterpret application views.
New gate logs include numeric flags/format/usage. The plugin and report expose
the gate refusal even when no bridge exists.

Tests cover eligibility rejection boundaries and an actual mutable WSI
swapchain with an alternate SRGB/UNORM image view, immutable caller metadata,
GPU LMC intermediates/pixel checks, and the extracted Decky launcher. This is
software-driver validation, not hardware acceptance for this game.

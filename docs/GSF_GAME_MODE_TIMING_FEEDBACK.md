# Game Mode timing feedback regression (2026-10-18)

The owner's `20261018T101015Z-game-en311kh7` and
`20261018T101119Z-game-900_n90m` reports run candidate `51f7a9b`.
The corrected layer order enables the GPU bridge in Game Mode. The owner
observed generated frames. Logs show distinct sampled pixels and accepted
intermediate output at approximately 100 ms (5→10), 16.7 ms (30→60), and
11.1 ms (45→90). Neither run passes the whole suite.

Low-rate cases terminate with stack protector failures after about 33–34
source pairs. This exposes a previously untested GOOGLE timing feedback
path: Desktop/lavapipe runs do not enable this compositor extension.

## Concrete compatibility defect

[Gamescope 3.16.16's implementation](https://github.com/ValveSoftware/gamescope/blob/3.16.16/layer/VkLayer_FROG_gamescope_wsi.cpp)
of `GetPastPresentationTimingGOOGLE` saves the caller's input capacity and
unconditionally erases that many vector entries, even when fewer were copied.
Our speculative capacity of 16 can therefore make it erase beyond the vector's
end. A bounded array and a bounded telemetry loop cannot prevent that damage
inside the downstream layer. Current Gamescope instead erases the returned
count. The exact Gamescope binary from the Deck has not been reproduced here;
this is a verified compatibility defect consistent with the crash, not proof
that every observed failure has the same cause.

The bridge now queries with input count zero and a null data pointer, then
requests at most the available count (and at most 16 records). The old erase
is safe for both calls. The bridge owns the GOOGLE ID namespace and holds the
same output WSI mutex across the query and fetch; it has no concurrent consumer
of this history. Additional arriving feedback can wait for the next poll.

`tests/past_timing.c` exercises the production reader with a legacy callback
that rejects erasing beyond available history: empty, sparse, 16/17/64/65/128
record backlogs, feedback arriving between calls, query/fetch errors and 3000
frames. CI also runs it under address and undefined-behavior sanitizers, and
checks the production desired-time packet builder separately.

## Remaining hardware acceptance

The 45→90 cases in the second report lose the X11 window and report
`VK_ERROR_OUT_OF_DATE_KHR`. Their cause is not established by these logs.
The first report is interrupted before completing this part of the matrix.
This change must not turn those failures into passes or equate WSI acceptance
with physical OLED scanout. A full Game Mode matrix on the Deck remains pending.

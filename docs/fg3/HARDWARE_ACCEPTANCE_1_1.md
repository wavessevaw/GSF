# Steam Deck acceptance scenario — NOT TESTED

No target Deck, DRM node or authenticated endpoint is connected to the current
cloud. The following is the reproducible next stage, not a request for another
user log and not a claim of completed acceptance.

1. Record Deck OLED/LCD model, SteamOS/Mesa/Gamescope versions, resolution,
   display refresh, game/Proton versions, internal limiter and Governor state.
   Target OLED1280×800/90Hz with an internal game30FPS cap. Do not cap the entire
   Gamescope output to30; do not change power/TDP to hide generator cost.
2. Install the checksum-verified experimental component or integrated Decky ZIP.
   Establish real-only baseline, then ×2 and ×3 runs. The standalone diagnostic
   entry is gfg-native --diagnose; the bundle records stderr, pairs.jsonl,
   system snapshots and report.md. Preserve baseline and FG sessions separately.
3. Confirm loader order GFG→FROG→ICD; the bridge must see app FIFO. Confirm real
   WSI refresh query, separate desired presentation IDs/times and compositor
   feedback. Driver MAILBOX below FROG is expected and must not be blindly forced
   into FIFO. Missing transport support is an explicit fallback/failure.
4. Test steady30sourceFPS, variable20–45FPS, refresh90→60→90, a long pause,
   identical repeated images, cut, fullscreen/resize, alt-tab and game exit.
   Old generations must retire and never supply images to a new swapchain.
5. Correlate generated pair identity and compositor completion with real DRM
   flips and preferably240+FPS optical capture. A present SUCCESS, generated
   label, estimated FPS overlay or GOOGLE completion alone cannot prove distinct
   panel frames. Independently check G1/G2 visual content and order; labels alone
   can make duplicate images appear unique and are insufficient.
6. Under a real game load capture GPU query times (analysis/model/local/reconstruct),
   source→worker age, readiness waits, WSI waits, deadline misses and drops.
   Keep unknown contention unresolved rather than attributing every miss to GPU.
7. Practical scenes: camera pan/fast turn, small moving object, crossing objects,
   disocclusion, HUD/subtitles, alpha/particles, lighting and cut. Save sequences;
   assess temporal artifacts and worst failures, not just mean PSNR. HDR PQ quality
   is currently unaccepted and must not inherit the SDR acceptance result.
8. At least10min baseline and10min FG in the same conditions, then sustained run:
   compare measured displayed/unique cadence, stutter, input-to-photon latency,
   power, temperature and crashes. Record failed/skipped cases and exact hashes.

A: software pipeline tested. B: native/GFG integration tested in cloud; real
Gamescope transport untested. C: Deck hardware NOT TESTED. D: practical acceptance
NOT TESTED. Stable release stays blocked until C/D evidence establishes the target
30real→90 actually displayed unique frames with acceptable quality and latency.

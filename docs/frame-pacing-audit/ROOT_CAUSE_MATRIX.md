# P0 root-cause matrix

| Cause | Code and evidence chain | Status |
|---|---|---|
| Nominal x3 fraction differs from reserved slot | frame_clock span2 emits mask2; prepare_generation used2/3;4433 archived slots are1/2 | Corrected and deterministic production-call replay passes |
| Admission window smaller than actual delivery+compute | retained84/168 calibrated joins;81/42 starts already late | Confirmed timing obstacle; pure queue cause not isolated |
| In-place telemetry truncation destroys transition history | launch.py truncate(0) above32MiB; both reports mark telemetry_truncated | Replaced by writer-owned line-boundary segmentation and hashed prefix export |
| Completion sample collection placed in constructor | calloc contextNULL makes guarded te_poll unreachable | Moved nonblocking poll to each present entry |
| B timing deliberately zeroed | output_present uses !pace_real/pressure; shadow records sent value/reason | Confirmed policy; not attributed to Gamescope failure |
| Adaptive ready callback may move slots after interpolation phase chosen | adaptive_pair gsf_ap_ready vs prepare_generation timestamps | Corrected: baseline targets immutable; late output expires; buffered owner remains separate gate |
| Early source pressure cancels optional work before long GPU delivery completes | output_worker sets pressure on next source; readiness loop cancels | Protective behavior; a bounded buffered alternative requires ownership redesign |
| Counter/CI mistaken for physical proof | WSI accepted and compositor timestamps do not identify panel pixels | G3 remains unqualified until independent15W hardware test |

This draft starts with G0 and safe independent fixes. It must not be described
as a completed continuous45→90 or30→90 solution while the remaining gates are open.

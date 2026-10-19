# 0.1.7: include measured submit latency in generation admission

## Hardware evidence, 2026-10-19

Game archive `(4)` used the 0.1.6 PR-test merge `0440b21fc8694f5676526b0921517ff6e96e8ed0`
(parents: 0.1.5 main and the final PR #33 head). It contains 8,034 source-pair
records and one process/bridge. Re-analysis with the scoped reporter joins
all 1,410 GPU samples and correlated records without ambiguity. Of those,
1,409 have valid calibration; one inconsistent timestamp sample is excluded.

| Measurement | Median | p95 |
| --- | ---: | ---: |
| TE submit return → calibrated GPU start | 35.699 ms | 50.406 ms |
| GPU start → first generated output ready | 7.074 ms | 9.610 ms |
| Per-pair sum of those durations | 43.585 ms | 58.654 ms |
| Time remaining from submit return to readiness deadline | 11.831 ms | 17.575 ms |
| Calibration max deviation | 10.091 µs | 14.611 µs |

These durations are not a measurement of pure queue wait. Dependencies,
command delivery and GPU scheduling are included. The passive GPU sampler's
median is 99%, for the whole GPU, not specifically the game or GSF. There is
no comparable layer-off game baseline, so source-FPS loss caused by GSF is
not quantified by this capture.

Of 1,412 dispatched synthetic outputs, 197 were WSI accepted (13.95%). All
1,412 watched timeline thresholds were eventually observed; none expired,
failed submission or reported device loss. In the calibrated subset after
the first 60 seconds, all 826 outputs were ready after their deadline, 824
started GPU work after that deadline, and none reached WSI. Completed work is
therefore not equivalent to timely output.

The report's exit code `-2` denotes SIGINT. It does not itself establish a
Vulkan crash. The observed run contains no bridge device-loss error. Physical
display cadence and uniqueness remain unverified.

## Confirmed admission defect

`gpu_budget_ns` tracked the GPU timestamp span beginning at query zero.
`prepare_generation` compared that span with remaining deadline slack. The
measured time before query zero was excluded. Sparse visibility probes could
also bypass a stale cost every twelfth source frame, repeatedly dispatching
work that was unlikely to arrive on time under this load.

For the 1,409 paired calibrated observations, the existing 0.5ms margin and
span-only test classify 1,378 as fitting their recorded post-submit slack.
Including measured start delay classifies 209 as fitting; 1,169 span-only
approvals are impossible under the measured complete duration. This is an
offline **deadline-feasibility audit**, not a causal scheduling replay or
prediction of performance after removing jobs. The anonymized duration-only
fixture is `engine/gfg-native/tests/fixtures/deck-calibrated-slack-20261019.csv`.

## Runtime change

For a trustworthy calibrated observation, future admission uses
`submit_to_gpu_start_ns + ready_ms[output]`, with the existing 0.5ms margin.
The calibrated estimate comes from a completed job; it is a cost estimate,
not a guarantee about the next job. Invalid observations do not erase the
last trustworthy delay and reopen a burst. New valid observations replace it,
allowing normal admission as soon as measured cost fits again.

The existing bounded two-second recovery retry is retained. Once calibrated
latency is known, visibility probes obey that same limit and cannot revive an
expired slot. A retry may rebuild fresh history first, then submit one output
at an eligible slot; it is counted as work, never as successful display unless
WSI accepts it. An old conservative estimate can thus recover without a game
restart. Sparse recovery is deliberately allowed to test changed conditions,
even when the old estimate predicts insufficient time.

No extra GPU waits, sleeps, pixel readbacks, queue depth or per-frame flushes
are introduced. `gfg_frame_clock_pair`, LMC and source-FPS policy are unchanged.
Absent reliable calibration, the previous span-only behavior remains available.
This means the protection is limited on devices without calibrated timestamps.

`pair.submit_delay_budget_ns` and `submit_delay_budget_calibrated` expose the
cost estimate actually available to admission. `gpu_budget_ns` retains its
old GPU-span meaning. TE public ABI remains unchanged.

## Controls and acceptance

Decky 0.1.7 defaults to `GFG_NATIVE_LATENCY_ADMISSION=1`. The panel's
`normal-no-latency-admission` control sets it to zero while retaining timestamp
calibration and the watcher, isolating admission from calibration overhead.
`normal-no-calibration` cannot exercise calibrated admission and must not be
treated as an equal-policy overhead control in 0.1.7.

Deterministic tests cover delayed start, 0ms delay, invalid calibration,
overflow, expired slots, retry limits and recovery. Tests of the actual
`prepare_generation` verify zero-output history signals, nonblocking TE packets,
single-output retry and reopened healthy 2× admission. The duration fixture
runs in native/Decky CI. Existing software Vulkan, pixel, variable 30–45Hz,
source-priority and swapchain lifecycle checks remain required.

Hardware acceptance still requires a comparable scene/thermal state: confirm
source frametimes are protected, futile dispatches decrease, and generation
recovers when headroom returns. More real frames with zero generated frames
is a fallback improvement, not proof of a working 2× multiplier.

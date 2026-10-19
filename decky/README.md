# GSF Motion Intelligence 0.1.13 Experimental

Frame Generation: Off / ×2 / ×3 / ×4. Reconstruction: Ultra Lite / Balanced /
Quality / Adaptive. All outputs share one pair analysis. Generated FPS counts
GPU-completed synthesis; Output FPS counts WSI-accepted presents, not OLED scanout.

Frame Buffer 0/1/2 is persisted per game and applies to the independent output
owner when its Vulkan contract is supported. That owner is currently opt-in for
barrier-based SDR applications; general Gamescope/Proton compatibility is still
pending. Buffer 0 adds no extra source period beyond causal A/B lookahead; Buffer 1/2
add one/two periods on the display grid. The ordinary WSI transport remains the default Decky path.

See `docs/motion-intelligence-0.1.13/ARCHITECTURE.md` and
`DECK_PROTOCOL.ru.md` in the source repository for limitations and hardware tests.
This ZIP is a compatibility preview, not a qualification of the full specification
or a guarantee of 90 FPS / 10 W. Keep the stable version available.

## 0.1.12 frame pacing audit candidate

Phase follows the actual reserved A/G/B slot (x3 mask=2 on 45→90 uses tau=0.5).
All paced outputs, including recovery probes, retain their original deadlines.
The opt-in x2 recovery policy no longer moves G1 or B after dispatch and adds no
B scheduling extension. Two observed GPU-ready + WSI-accepted outputs are
required for STABLE; a retry timer alone is never success. State telemetry is
available in normal x2/x3 modes too. The shadow classifier remains passive.

Segmented frames JSONL is enabled by the installed launcher. The writer keeps
earlier segments and the export includes exact hashes and a loss manifest.
The separate real-LMC output-owner proof is test-only and excluded from this
plugin. This package does not establish sustained OLED90 or 15W hardware
qualification. See `docs/frame-pacing-audit/FINAL_REPORT.md` and PR45.

The sections below preserve the previous experiment history; their 2-refresh
B-extension and ready-time retarget behavior is superseded in this candidate.

## x2 / x3 frame-generation mode (GSF 0.1.11)

Select multiplier per game in Decky. x2 = at most one interpolated output per source pair; x3 = at most two. The underlying frame clock already schedules only free display slots: at 90 Hz, x2 45→90, x3 30→90, either mode 60→90 can admit a single G1 on alternating pairs. Switching game base FPS does not require changing multiplier, although switching the multiplier itself requires restarting the game. The game and SteamOS display should both be set to 90 Hz for the requested target.

**Safety:** GSF does NOT force the refresh to 90 Hz if Gamescope reports 60 Hz. `output_refresh_hz` and `refresh_origin` in live telemetry distinguish a verified WSI refresh query from the configured 90-Hz fallback. Source FPS is never capped by the plugin. Never generate beyond available slots, never delay real frames for synthetic content, and never promise physical scanout from WSI counters.

This package adds x3 Game Mode instrumentation and end-to-end lavapipe smoke in addition to the existing x2 path. Exact x3 quality/cadence on Steam Deck OLED is **not yet verified**.

# GSF 0.1.11 LAB: Adaptive output-slot scheduling with per-game 2x / 3x

0.1.10 changes only the lifetime of the current-pair native-B pacing flag: it is reset on entry to every real pair so an early gpu-budget bypass cannot inherit the previous pair's successful G1 target. No other admission or timing policy changes. Shadow telemetry is on by default for normal and adaptive 2x; select the control mode without Shadow to measure its overhead. Restart after choosing a mode.

Each shadow_admission event records a pre-submit GPU-start prediction from already observed calibrated GPU timestamps (threshold 5 ms, sample freshness 500 ms), 3/5 medians, sample age and a hypothetical probe after 16 negative decisions. No extra GPU work is submitted, and a shadow skip never suppresses the existing TE-submit.

The same event captures the pre-submit B target, post-GPU-ready target, exact target at WSI call and desiredPresentTime actually sent to vkQueuePresentKHR. Zero reasons: 1 timing not attached, 2 diagnostic, 3 probe, 4 no accepted G1, 5 source pressure, 6 other, 0 nonzero. present_timing_supported indicates the GOOGLE device extension was enabled, not a property of surface capabilities. A requested time and compositor feedback are distinct; neither guarantees physical scanout.

Existing packed10 guards, Vulkan ownership, real-frame priority, and LMC remain unchanged. This is a stacked experimental PR and not a verified 2x Deck release.

# GSF 0.1.8 LAB: Adaptive pairs and stream reacquisition

This is a **research-only Decky candidate**, built on the stacked PR #38 and #40 diagnostic audit; main and the packed10 guard remain unchanged.

Select **Эксперимент 0.1.8: адаптивная пара G1/B** in the Decky diagnostic dropdown (restart the game). Normal mode retains the existing 0.1.7 timing and latency-admission policy.

Opt-in flag GFG_NATIVE_ADAPTIVE_PAIRS=1 activates on 2x same-device GPU LMC only. The policy forecasts finish from calibrated submit-to-GPU-start and TE GPU span, tentatively reallocates a G1 slot while limiting real B's *scheduled* extension to 2 refreshes. After observing GPU completion through the existing timeline wait, it re-selects an actual G1/B ordered pair. Source pressure, lateness and no viable pair cause real-only fallback. It never moves waits into the application Present thread and never holds an unlimited queue. The native output owner calls remain G1 then B.

Adaptive recovery states: SYNCING, GENERATING, DEGRADED and REACQUIRE. A failed transient pair does not require restarting the game. A stale calibrated estimate cannot disable the game session forever: a bounded re-acquisition probe is allowed after a backed-off quiet period. Each bridge has its own state, and GSF writes `adaptive_pair` JSONL events. The test checks healthy generation, impossible-late G1, bounded-B scheduling, pressure, discontinuities and repeated recovery under 2200 variable-source steps.

**Evidence limits:** This is a host-side slot policy, NOT direct GPU-input-ready observation. It observes when generated G1 becomes ready, but cannot make an input image ready earlier or guarantee that the compositor displays the requested timestamp. Two refreshes constrain planned native-B *scheduling*, not a guarantee on driver/Gamescope execution latency. Existing packed10 gating is not relaxed, and 0.1.8 has no hardware 2x validation. Never interpret WSI acceptance as physical scanout.

# GSF Decky Test 0.1.7 — calibrated latency admission

0.1.7 includes trustworthy submit-to-GPU-start latency in the synthetic-work
budget. Once calibrated latency is known, visibility probes share the bounded
two-second recovery retry instead of bypassing an impossible budget every
12 source frames. Fresh measurements reopen normal generation when it fits.
No source FPS floor or game limiter is imposed. This protects source output;
it does not establish sustained Deck 2×.

The control `normal-no-latency-admission` retains calibration/watch telemetry
but selects the previous admission policy. `normal-no-calibration` also removes
the calibrated input to admission and is no longer an equal-policy overhead
comparison. Pair telemetry exposes `submit_delay_budget_ns` and
`submit_delay_budget_calibrated`. See the repository's
[0.1.7 evidence and acceptance](../docs/GSF_CALIBRATED_ADMISSION_0_1_7.md).

# GSF Decky Test 0.1.6 Diagnostic (GPU calibration + AMDGPU sensors)

REPORT.json includes calibration_status_counts, actual calibrated submit_to_gpu_start_median_ms and per-window global GPU busy; invalid calibration samples remain null, never assigned to queue wait. No game-specific GPU utilisation or source-FPS regression can be inferred without controlled baseline.

Opt-in VK_KHR/VK_EXT_calibrated_timestamps is enabled on the application's Vulkan device only when supported. For GPU pairs, te_submit_frame captures CLOCK_MONOTONIC directly after successful queue submission and GPU raw query 0 is exported by te_sequence. The field **submit_to_gpu_start_ns** is not a pure queue-wait measurement. Calibration is attempted every 10 generated frames or 100 ms, with per-pair maxDeviation gating (<=1ms), stale pair detection and null fields when untrustworthy. All timestamps are diagnostic, no change to frame_clock, LMC, admission or timeouts.

A launcher-side Python sampler records available /sys/class/drm/card*/device gpu_busy_percent, pp_dpm_sclk, power1_average and temp1_input every ~100ms in amdgpu.jsonl without root. Data is global GPU usage, not game-specific; ambiguous/missing GPU yields unavailable. Game Mode diagnostics include normal-no-calibration and normal-calibration-every-frame for A/B overhead checks.

GPU samples are joined to the pair that actually submitted synthetic work using
`(pid, generation, te_sequence)`. `pair.te_submit_sequence` is zero when that pair
did not submit TE work; the legacy `te_sequence` remains a cumulative counter.
Bypassed pairs cannot move an earlier job to a later reporting window. Older
records without a complete namespace are joined only when their origin is
unique. `gpu_sample_joins` reports matched, unmatched and ambiguous records for
each GPU event type; ambiguous samples remain absent from per-window medians.
Global calibrated medians still describe all valid samples, including those
whose reporting window cannot be identified.

# GSF Test 0.1.5 Diagnostic — Decky, Game Mode

**0.1.5 measures completion of real LMC GPU jobs after WSI missed its deadline.**
It does not change frame_clock, shader implementation, admission or output timeout. New `GFG_NATIVE_TIMELINE_WATCH=1` (in Game Mode) records per-te_sequence readiness and 200ms observation limits, and 3 control modes can be configured from the Decky panel (restart game).

`timeline_watch` telemetry records `wait_threshold`, `actual_signal_value`, `submit_status`, `deadline_kind`, `watch_end_reason`, `timeline_observed_ns`, `last_below_ns`, `watch_poll_interval_ns`. A `threshold_reached` event is a CPU observation of a monotonic Vulkan semaphore threshold, **not** independently verified execution of a particular command when signals skip values, GPU completion timestamp, WSI acceptance or physical OLED scanout.

`normal-no-watch`: identical normal 2× path with watcher disabled for overhead A/B.

`source-only`: preserve original presents through worker without TE synthetic generation. `compute-only`: existing unthrottled compute-only stress test. `matched-compute`: use normal generation admission policy without synthetic readiness wait or presentation; comparisons are NOT equal-load scans when Gamescope frame rate changes.

A/B benchmarks must use one saved scene and comparable thermal state; watcher overhead must be measured using WATCH=0 vs WATCH=1 under identical game workloads. Auto-generated `REPORT.json` includes `diagnostic_mode` and a watcher completion summary. Timeline watch only covers same-device GPU LMC (not CPU-fit or shared-fd).

# GSF Test 0.1.4 — Decky, игровой тест 2×

**0.1.4 — ограниченная проверка реального LMC-кадра в игровом WSI.**
В тестовом режиме установлен LMC tier 2 (сокращённый поиск движения),
чтобы снизить GPU-стоимость; качество интерполяции может быть ниже tier 0. Раз в 12 исходных
кадров при наличии промежуточного дисплейного слота и отсутствии очереди
делается короткая попытка GPU LMC, даже если обычная оценка GPU-cost отклонила
синтетику. Ожидание готовности GPU ограничено 12 мс, получение WSI-образа
ещё 2 мс. Кадры, не прошедшие фактическую готовность и WSI, отбрасываются.
`visibility_probe` в game.log показывает `synthetic_wsi` и `real_wsi`.
Это не гарантия 2×, не проверка физического scanout и не исправление
GPU-конкуренции. Приоритет исходных кадров сохраняется.

## Изменения 0.1.3 и 0.1.2

Экспериментальный GPU LMC для Steam Deck OLED. Пакет работает автономно,
содержит GPU слой и исходники, ничего не скачивает при включении.
Поддерживаемый первый сценарий: игра x64 через Vulkan, DXVK или VKD3D-Proton.
Игра и конкретный Proton ещё требуют аппаратной проверки.
0.1.3 убирает ожидание единственного слота очереди и занятого GPU-буфера.
При накоплении исходных кадров генерация уступает им время вывода; исходные
кадры сохраняются в порядке отправки. Подробный лог сбрасывается пакетами.
Это исправление задержек, а не подтверждение устойчивых 2× на Steam Deck.

0.1.2 добавляет проверенные UNORM/SRGB mutable-format swapchain DXVK.
Если GPU слой отказался подключить генерацию, панель показывает причину отказа;
одна лишь команда запуска не считается подтверждением работающего генератора.

1. В настройках Decky включите режим разработчика и установите этот ZIP через
   **Install Plugin from ZIP / Установить плагин из ZIP**. Не распаковывайте ZIP
   внутрь другого ZIP. Плагин называется **GSF Test**.
2. Откройте GSF Test в боковом меню, выберите установленную Steam-игру.
   Появятся её название и подтверждение выбора. Выбор сохраняется при закрытии
   панели и перезапуске Decky. Сам выбор ещё не меняет параметры запуска.
   Для отдельного эксперимента уберите запуск через GFG/MAKO/LSFG у этой игры.
3. Нажмите **Включить 2×** сразу под названием игры. Плагин установит слой в пользовательскую
   папку, сохранит прежние параметры запуска и проверит запись новых через API
   Steam. После подтверждения вы увидите точную записанную команду вида
   `python3 /home/deck/.local/share/gsf-test/launch.py --appid APPID -- %command%`.
   Существующие параметры игры сохраняются в этой строке. Ничего вручную
   вставлять не нужно. Перезапустите выбранную игру. На OLED для первого теста выберите 90 Гц.
4. Играйте с её обычным меняющимся FPS. GSF не задаёт игре цель 30/40/45 FPS,
   не меняет TDP, частоты GPU, разрешение и лимитер Steam. Режим 2× пытается
   добавить один кадр между исходными, пока хватает времени и слотов экрана.
   На 90 Гц выше 45 исходных FPS полный 2× не помещается: часть генерации
   пропускается, оригинальные кадры продолжают идти.
5. Смотрите состояние в плагине. «2× настроен» означает только параметры
   следующего запуска. «Генерация кадров подтверждена» появляется по живым
   счётчикам GPU слоя, а не по положению переключателя.
6. После выхода автоматически сохраняется
   `~/GSF-Test-Results/GSF-Game-Latest.zip`. **Сохранить отчёт** делает снимок
   текущего/последнего прогона. Отчёт никуда не отправляется автоматически.
7. **Восстановить запуск без GSF** возвращает прежнюю строку запуска, если
   вы не изменили её самостоятельно. После изменения нужен перезапуск игры.

Это тест генератора в настоящей игре, не гарантия совместимости каждой игры.
Счётчики обозначают кадры, принятые WSI, и не равны доказанному физическому
FPS панели. На этом этапе есть нерешённый риск потери окна/swapchain в Game
Mode; такие сбои сохраняются в отчёт, а не объявляются успешным тестом.
Автоматический тест использует реальный Vulkan и LMC на программном драйвере;
он не заменяет Steam, Proton или OLED.

При удалении плагина остаётся маленький отключённый launcher: Steam может
ссылаться на него, и он пропускает запуск игры без GSF даже без GPU файлов.
До удаления удобнее восстановить параметры кнопкой в плагине.
Журналы текущего запуска ограничиваются примерно 16 МиБ для game.log и
32 МиБ для JSONL; при обрезке это явно записывается в отчёт.

GSF: GPL-3.0-or-later. Исходники этой сборки — SOURCE.tar.gz, список пакетов
сборки и точные версии — decky/package-lock.json в исходниках. Decky UI и
React предоставляет установленный Decky Loader. Исходники и лицензии
использованного API/loader shim приложены в third_party.

The opt-in `normal-queue-observe` diagnostic records application submit completion evidence at TE enqueue. It preserves normal admission and calibration. Counts are upper bounds, not hardware queue depth; see `docs/GSF_QUEUE_OBSERVATION_DIAGNOSTIC.md`. The normal profile leaves it disabled. Launcher-prefixed GFG_NATIVE variables are intentionally filtered; select the diagnostic profile instead.

## 0.1.10 causality gates
- Shadow computes before budget/pressure or other early bypasses, and its labels do not suppress TE. `admit_probe` after 10 hypothetical skip/unknown decisions, gated by 500ms cooldown, is a diagnostic label ONLY and never triggers extra GPU work. GPU-start observations remain sampled only when available.
- `B_pacing_active_at_present`, `B_display_timing_mode_at_present`, `B_clock_period_ns_at_present`, `B_worker_path_at_present`, `B_pace_real_at_present`, `B_output_pressure_at_present`, `B_diagnostic_at_present`, `B_probe_at_present` capture the exact invocation state.
- Zero `desiredPresentTime` does not imply missing Vulkan extension. In normal Game Mode, `B_pace_real_at_present=0` or `B_output_pressure_at_present=1` deliberately requests immediate real presentation. Present feedback is NOT a measurement of GPU-submit-to-start latency.
- No confirmed game-mode frame generation or scanout claims from lavapipe-only CI. `normal-no-shadow` is the matched control mode.

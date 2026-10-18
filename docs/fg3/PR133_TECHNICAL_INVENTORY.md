# КТ-1: фактическое состояние PR #133

Исходный snapshot: `5782aa992b4c2509210fddd57759933a8c6bc6d8`.
Main после fetch: `85092856107143fa3aa93d1dd926ba7d9cabf231`;
merge base: `a764fd1483ca81c909317256ab8c2b98ca266a35`.
Полный PR diff: 237 файлов, 24488 добавленных / 250 удалённых текстовых строк.
Это не diff к устаревшему local main: он ошибочно включал 411 файлов.
Полный перечень, категории и SHA-256 каждого нового blob:
[evidence/pr133-inventory-5782aa9.json](evidence/pr133-inventory-5782aa9.json).

## Production call map, проверенная по коду

```text
Vulkan loader -> native_CreateInstance -> native_CreateDevice (layer.c)
  -> native_CreateSwapchainKHR (layout/usage/mode gate, image budget)
  -> native_AcquireNextImage[2]KHR (acquired index/sem tracking)
  -> native_QueuePresentKHR
       -> gfg_gpu_fg_create [первое использование swapchain]
          -> te_device_borrow [app VkDevice + app queue]
          -> te_context_create [TE_ALGO_SHARED_FLOW, variant4]
       -> gfg_output_submit [immutable metadata + submit app waits -> private gate]
       -> output_main [один executing + один queued job]
          -> gfg_gpu_fg_present
             -> record_real / submit_real [GPU capture -> source history]
             -> te_submit_frame
                -> lmc_record_analysis [pair-shared GPU analysis]
                -> lmc_record_output(0,t=1/3)
                -> lmc_record_output(1,t=2/3)
             -> present_synthetic [GPU copy/blit, distinct output image]
             -> output_present -> native_serialized_present -> lower WSI
             -> forward_real [original real image, original real-only metadata]
```

Runtime CPU: управление ресурсами, запись command buffers, bounded queue,
синхронизация host доступа к VkQueue/WSI, control-header telemetry и pacing.
Runtime GPU: capture, pyramids, bidirectional search, affine/palette/projective
model, visibility/orphan refinement, два SELECT/COMPOSE, final copy/blit.
CPU pixel staging в GPU runtime не используется. `verify` и observer читают
пиксели только в диагностике/тестах. Собранный production ELF проверяется packager
на отсутствие test observer и legacy CPU bridge.

## Подтверждённые контракты и ограничения

- G1/G2 создаются отдельными dispatch с разными t и destination images. Общий
  analysis не повторяется. Это реальные разные изображения, не aliases.
- Обе реконструкции submit заранее в одну app queue. Copy G1 стоит после compute
  G2; admission дополнительно ждёт весь TE context ради cut/control header.
  Поэтому separate compute timestamps не означают независимый ранний вывод G1.
- Source images имеют parity-history, GPU timeline V/V+1/V+2, ring fences.
  Gap сбрасывает predictor. Swapchain owns bridge/worker; destroy/idle drains
  precede destruction. Явного сквозного generation/pair ID ещё нет.
- Worker копирует ID/fence/timing chains; caller pointers не удерживаются.
  App waits submitted до возврата Present. Per-image gates переиспользуются
  через reacquisition. Unknown chains идут через drain/pass-through.
- Clock фиксирует три слота с периодом `GFG_NATIVE_OUTPUT_HZ`, стартует по времени
  worker. Реальный interval между source frames не входит в модель. Частота
  display не запрашивается и refresh change не адаптирует clock.
- `queued_ns` устанавливается заново после backpressure; это не capture age.
  Старение до admission теряется. Источник/пара не имеют временных метаданных.
- Late G1 прекращает весь output loop, даже если G2 ещё успел бы. Compute и
  metadata waits смешаны с transport/clock decisions; 90 Hz причины не локализованы.
- Native mode допускает только FIFO. Нижний Gamescope WSI может предъявлять
  MAILBOX как внутренний transport. Это подтверждено просмотренным Gamescope
  source, но фактическое layer ordering на устройстве не установлено.
- VK_GOOGLE_display_timing query functions только forwarded; extension не
  provisioned генератором, собственные future timestamps не задаются.
- HDR packed10/FP16 transport есть, но интерполяция PQ code values не linear HDR.
- Глобальные bridge counters считают WSI accepted calls. Они не дают physical
  FPS и не позволяют восстановить события конкретной пары.

## Что в PR не является production-путём GSF

`cpu_bridge.c` — legacy opt-in; MAKO adapter — отдельный исторический путь;
`bench`, reference Python, traces и quality tools — не игровой runtime.
`GFG_NATIVE_TEST_OBSERVE_LAYER` подменяет lower WSI test observer, читает pixels
и делает GPU waits; его ELF packager запрещает. Xvfb — software WSI fixture.

Plugin TemporalEnginePanel/temporal_engine.py относятся к MAKO status schema
(алгоритмы tiles/lmc), не к native GSF. Native GSF RPC/status/settings отсутствуют.
Package launcher по умолчанию включает lmc-gpu и FORCE_FIFO=1, в отличие от
GSF dev launcher. Поэтому включение нового генератора из GFG ещё не завершено.
Остальной diff включает TE legacy migration, diagnostic assets, build/package
scripts и frontend отображение LMC costs. Эти изменения не доказывают GSF runtime
integration; актуальные Governor power/cap loops не нуждаются в переписывании.

## Границы 13 существующих checks

1. `core-gsf`: 182 assertions engine/scheduler, static exactness, textured pan,
   HUD, timeline, hold/discontinuity, noise-cut. Не WSI или Gamescope.
2. `borrowed-gsf`: textured motion, BGRA exactness, app-device/timeline lifetime.
3. `sync-real-only-fallback`: без worker generation не запускается.
4–13. Два порядка layers × пять сценариев: actual WSI color interpolation/order/
   spacing; black-white cut без synthetic credit; recreate; queue idle; unknown
   metadata. Сценарии имеют 4/6 real frames и 96×64 software surface.

Color WSI observer использует uniform clears, а textured warp проверен отдельно
в borrowed-device test. Нельзя утверждать на этой основе качество движения в игре,
прозрачностей, частиц, переменного FPS, refresh change или аппаратные 30→90.
В snapshot clock9Hz задавался явно, но test app отправлял source каждые33мс;
описание «source3Hz» в старом отчёте не подтверждено. Этот дефект fixture
исправляется явным sourceHz и запретом catch-up burst. Не performance proof.

## Решение по обнаруженным разрывам

Сохраняется выбранная архитектура. Доработка в порядке критического пути:
1. Source timestamps/pair+generation IDs, variable interval и настоящий frame age.
2. ×2/×3, refresh-aware admission, per-output deadline decisions/telemetry.
3. Standard future presentation times и проверяемое положение над Gamescope WSI;
   не включать blanket FIFO override как скрытую замену compositor integration.
4. Native GSF settings/status и launcher/package consistency без изменения TDP.
5. Functional/negative regression, installable experimental artifact и level C
   NOT TESTED при недоступном Deck. Крупная quality optimization пока не начинается.

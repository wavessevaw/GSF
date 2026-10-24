# Hardware Runtime: DirectScan, completion/deadline и C1

Экспериментальная сборка поверх Kedavra 1.4.1. Все новые режимы **Off по умолчанию**.
Исходный и фактически собранный commit записаны в BUILD-INFO.json. Это не релиз и не
подтверждение ускорения: реальные GPU ms, watts, latency и физические пиксели на Deck ещё не измерены.

## Что реализовано

1. **DirectScan SDR (Sync0).** DRM backend сначала вычисляет обычные причины обязательной
   композиции. Если они отсутствуют, Kedavra готовит GEN/подходящий REPEAT до решения о
   scanout. Последний warp пишет прямо в sampled/storage/flippable ABGR8888 image,
   выделенный штатным Vulkan allocator Gamescope с его пересечением DRM/Vulkan modifiers.
   Три slots проверяются через `CVulkanTexture::IsInUse()` (включая backend FB refs queued/visible
   и GPU consumer refs) и completion writer sequence. Занятые slots не перезаписываются;
   resize может заменить только свободный slot. DRM, liftoff, Mura/HUD/Steam overlays,
   atomic validation, commit и retirement остаются у Gamescope. Partial composition overlays
   разрешена прежним backend. При fallback подготовленный base используется повторно без
   второго warp. REAL кадры пока идут через обычную композицию. HDR input/output, shader
   rotation, FSR/NIS/SGSR, effects и остальные штатные ограничения сохраняют composition.
   Перед KMS сохранён CPU wait **на writer sequence**. Nonblocking IN_FENCE handoff в этот
   вариант не входит; удалён финальный полноэкранный BLIT eligible base, а не этот wait.
2. **Deadline-aware analysis.** Планировщик получает готовность пары после наблюдаемого
   `completedSeqNo()` финальной analysis submission. Job содержит history epoch, source ID,
   sequence и CPU submit time. Reset/resize/focus/policy смена инвалидируют старые jobs.
   Включённый режим не сообщает заранее `latch+2`. Admission резервирует время следующего
   final кадра и guard, используя консервативную оценку измеренной GPU-region стоимости
   анализа и final пути плюс CPU submit cost. При нехватке slack работа откладывается.
   Устаревший **неотправленный** остаток отменяется при новом source вместо переноса внутрь
   final composite. Незавершённая пара не используется как temporal seed. Это эвристика
   userspace одной очереди, без гарантированного hardware deadline и без новых priorities.
3. **C1.** Финальный SDR post pass пишет в отдельный history target вместо post→history copy.
   Dimensions/format/ownership/aliasing guards сохранены. Без фильтров C1 не экономит pass.

## Всё управление в плагине

Установить внутренний ZIP через обычный интерфейс Decky. В GSF Kedavra:

- Setup → Install/Reinstall test runtime → Reboot now.
- Выбрать Auto/Custom и base FPS на главной панели.
- Setup → DirectScan, Deadline-aware analysis, C1 direct history: независимые переключатели.
- Для C1 выбрать Image filters (например Sharp) и держать один preset во всех A/B/A.
- Setup показывает requested flags, reasons и отдельные prepared/commit/KMS flip counters.
- Setup → Run pixel self-test: проверка texel parity на текущем Vulkan ICD; запускать из
  меню Steam, поскольку тест создаёт GPU нагрузку. Результат автоматически сохраняется в отчёт.
- Setup → Save report to Desktop после проблемы. Терминал и ручное чтение файлов не нужны.
- Возврат к baseline: выключить все три переключателя. Возврат к системному runtime:
  Remove and restore stock Gamescope → Reboot now.

Настройки сохраняются и восстанавливаются плагином. Старый/stale runtime не принимает
включение новых режимов. Ошибка команды или сохранения вызывает откат к прежним настройкам;
ошибка самого отката также журналируется.

## Автоматическая диагностика

Постоянный `runtime-events.jsonl` и две ротации по 512 KiB содержат UTC, backend session,
CPU monotonic time, запросы/результаты/ошибки настроек, rollback и sampled runtime status.
Status опрашивается раз в секунду; изменения пишутся сразу при наблюдении, heartbeat — раз
в пять секунд. Это **снимки наблюдений**, не полный покадровый журнал. Счётчики сохраняют
сведения о повторяющихся отказах между снимками. Ограниченный объём не является архивом всей
многочасовой сессии: сохранять report после обнаружения проблемы. Ошибка записи лога видна в UI.

Отчёт включает logs/rotations, settings, current summary, plugin/installed manifests,
результат self-test, system journal, GPU reset/coredump diagnostics и имеющиеся pacing traces.
Log в плагине показывает и runtime events, и system journal.

Причины: `filters_off`, `hdr_fallback`, `post_allocation_failed`, `target_alias`,
`plan_redirect_rejected`; DirectScan: `backend_requires_composition`, `hdr_output_composition`,
`no_scanout_modifier`, `scanout_pool_busy`, `scanout_allocation_failed`,
`atomic_plane_validation_failed`, `atomic_commit_failed`; analysis: `insufficient_deadline_slack`,
`superseded_before_submit`, `submitted_waiting_completion`, `completion_observed`.
Штатный fallback — не ошибка GPU. Ошибки allocation/commit выделяются отдельными counters;
после allocation/commit failure retry ограничен паузой пять секунд.

`direct_scan_prepared` — commands + writer wait; `commits` — accepted atomic request с FB
из пула; `flips` — matching KMS event с present ID и FB из контекста принятого commit.
Даже такой flip **не доказывает**, что содержимое пикселей изменилось: нужен optical/content test.
GPU elapsed, CPU submit и CPU completion-observed timestamp не смешиваются в «GPU execution».

## Прогоны и приёмка

CI: ASan/UBSan tests production admission/completion/retirement helpers, 40000 vblanks
completion/reset stress ×2/×3; существующая scheduler matrix; Vulkan correctness и C1 exact
parity на lavapipe; backend fault injection, persistence/rollback, rotation/report packaging;
TypeScript и сборка patched Gamescope с runtime smoke/older-glibc compatibility.

На Deck ещё обязательны отдельные A/B/A: все Off; только C1; только completion/deadline;
только DirectScan; затем совместно. 45→90 ×2, 30→90 ×3, 30→60 ×2; дополнительно 24→90.
Сохранять одинаковые game route/filters/HUD/Mura/HDR/power settings. Проверять Steam menu,
notifications, HDR switch, resize, suspend/resume, hotplug, shader rotation и forced composition.
Нельзя выключать HUD/Mura ради объявления успеха. Проверить retirement при повторах/failed
commit; сопоставить counters с GPU trace и внешним content measurement. Сравнивать GPU
region time, game real FPS, missed updates/content age, pacing tails и энергию всей сессии.
При GPU reset, corruption, black screen или регрессии немедленно выключить новые режимы и
сохранить report. В этой среде Steam Deck/DRM hardware тестов нет; CI их не заменяет.

# GFG Native FG — исследование

ТЗ: [исходный запрос](SPECIFICATION.md). Исследовательская ветка `codex/native-fg-research`, основа GFG te.8 `a7f424af20cfb56efdbddf3c4e8e4bde414f3c0d`. Дата: 15 октября 2026, Asia/Vladivostok. Этот пакет не является новым релизом генератора.

## Состояние этапов

| Gate | Статус | Доказательства / недостающие данные |
| --- | --- | --- |
| 1 LSFG | OPEN | Есть открытый host-side контракт MAKO; оригинальная Windows-программа и её DLL здесь не исследованы. Нужны локальный reference run и Windows-трассы. |
| 2 MAKO | OPEN | SOURCE VERIFIED: граф вызовов, FD/timeline, reuse, pacing и изменения upstream. Нет Deck timestamps для полной стоимости. |
| 3 GFG audit | OPEN | SOURCE VERIFIED: CPU round trip, объёмы, зависимость инициализации от DLL, пробелы метрик. Нужны сопоставимые аппаратные прогоны. |
| 4 Architecture | NOT APPROVED | [Кандидатный контракт и эксперименты](NATIVE_FG_ARCHITECTURE.md), выбор алгоритма отложен. |
| 5–9 | NOT STARTED | Новый независимый backend, аппаратная приёмка и RC не создавались. |

Частичный source audit этапов 2/3 выполнен параллельно сбору исходных данных этапа 1; это не переход через незакрытые gates к реализации.

## Полученный аппаратный пакет

DEVICE VERIFIED (пользовательский запуск inventory, не удалённое выполнение): Galileo, SteamOS3.8.28, glibc2.41, RADV/Mesa26.0.0-devel. Vulkan enumeration успешен. [Сводка и хеш входного ZIP](results/device_inventory_20261015.json). LSFG reference и performance comparison в этом пакете отсутствуют; аппаратные gates остаются открытыми.

## Проверяемые выводы

- SOURCE VERIFIED: MAKO повторяет не только финальное смешивание. Общий prepass — 34 dispatch, цепочка каждого выхода — 66 dispatch для исследованного SDR-пути. Это сведения о вызовах, не измерение скорости. [Граф](LSFG_COMPUTE_GRAPH.md).
- SOURCE VERIFIED: LMC уже разделяет анализ пары и синтез выхода. Простое добавление reuse не устраняет его CPU-зависимость. [Аудит](GFG_CURRENT_ENGINE_AUDIT.md).
- SOURCE VERIFIED: продукт te.8 инициализирует DLL-зависимый backend до создания TE. Самостоятельная математика LMC не означает самостоятельный запуск продукта.
- SOURCE VERIFIED: текущий upstream MAKO имеет DXBC→SPIR-V fallback и диагностическое профилирование, отсутствующие в закреплённой основе GFG. Нельзя переносить результаты между этими версиями без проверки.
- NOT TESTED: превосходство над MAKO+LSFG, 2 мс/выход, энергопотребление, аппаратные 90 FPS, равноценное LSFG качество.

## Навигация

[Исполняемый host-call эксперимент:36 конфигураций](MAKO_HOST_CALL_EXPERIMENT.md), [статус прямого доступа](REMOTE_ACCESS_STATUS.md).

[Источники и SHA](SOURCES.md), [LSFG: бинарные границы](LSFG_BINARY_ANALYSIS.md), [граф](LSFG_COMPUTE_GRAPH.md), [шейдеры](LSFG_SHADER_ARCHITECTURE.md), [модели](LSFG_MODEL_ANALYSIS.md), [oracle](LSFG_REFERENCE_ORACLE.md), [стоимость](LSFG_COST_MODEL.md).

[MAKO: архитектура](MAKO_ARCHITECTURE.md), [жизненный цикл](MAKO_FRAME_LIFECYCLE.md), [стоимость](MAKO_GPU_COST.md), [синхронизация](MAKO_SYNC_ANALYSIS.md), [pacing](MAKO_FRAME_PACING.md), [ограничения](MAKO_LIMITATIONS.md).

[Подключение Deck](DECK_CONNECTION.md), [план аппаратных измерений](DEVICE_VALIDATION.md), [происхождение и лицензии](../docs/PROVENANCE_AND_LICENSES.md).

Статусы выводов: SOURCE VERIFIED — исходники; BINARY VERIFIED — собственный анализ конкретного бинарника; TRACE VERIFIED — конкретная трасса; SIMULATED — программный/синтетический стенд; DEVICE VERIFIED — целевое устройство; HYPOTHESIS — гипотеза; NOT TESTED — данных нет. Метка относится к конкретному утверждению, а не ко всему продукту.

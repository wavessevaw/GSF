# GFG: аудит текущего движка и прежней стратегии

Основа `a7f424af20cfb56efdbddf3c4e8e4bde414f3c0d` (te.8). Это source audit и план экспериментов, не завершённый аппаратный анализ стоимости.

## Подтверждённые ограничения

| Компонент | Вывод | Статус |
| --- | --- | --- |
| Native independence | InstanceImpl с DLL registry создаётся до Runtime::create TE. Ошибка DLL может остановить backend ещё до выбора LMC. Режим fallback сохраняет LSFG. Продукт не проходит независимый startup test. | SOURCE VERIFIED |
| LMC CPU path | GPU analysis → mapped readback → CPU fit → model upload → second submission. CPU находится на critical path этой пары. | SOURCE VERIFIED |
| Reuse | LMC уже делает analysis один раз и SELECT/COMPOSE на каждый t. Это не недостающее нововведение. | SOURCE VERIFIED |
| Readback | 1280×800: level1=640×400, блок4×4, n1=160×100=16000. Копируется `20*n1+4=320004` байта/пару; physical buffer alignment отдельно. Модель помещается в stage 512 B. | SOURCE VERIFIED |
| LMC work | Четыре pyramid levels, два grid searches, field/masked/orphan/field2; default tier0 и ocap>0: 50 analysis dispatch, затем 2/output. Tier1: 38+2N, tier2: 34+2N. Сокращение dispatch не доказывает выигрыш. | SOURCE VERIFIED |
| Image storage | Две R32 luma pyramids, две half и две field RGBA16F, R32 HUD: логический payload 23 200 000 B при1280×800. Это без buffer/import/output/alignment и не фактический VRAM. | SOURCE VERIFIED |
| Teardown | Worker повторяет 100 ms vkWaitSemaphores при VK_TIMEOUT; teardown дожидается worker. te.8 исправил readiness deadline, но не отмену зависшего producer и не bounded destruction. | SOURCE VERIFIED |
| Ready metrics | q0 ставится после возможного host-input copy/ownership; ready span включает CPU/queue gap после начала analysis, но исключает исходное ожидание, transport до q0 и presentation после выхода. | SOURCE VERIFIED |
| Shadow | Теневой контекст использует частные outputs параллельно primary. Его нагрузку нельзя интерпретировать как стоимость единственного выбранного backend. | SOURCE VERIFIED |

Ссылки: [engine/gfg-fusion/renderer.patch:258](https://github.com/wavessevaw/GFG-Extreme/blob/a7f424af20cfb56efdbddf3c4e8e4bde414f3c0d/engine/gfg-fusion/renderer.patch#L258), [engine/gfg-temporal/mako/gfg_temporal.cpp:735](https://github.com/wavessevaw/GFG-Extreme/blob/a7f424af20cfb56efdbddf3c4e8e4bde414f3c0d/engine/gfg-temporal/mako/gfg_temporal.cpp#L735), [engine/gfg-temporal/src/te_lmc.inc:113](https://github.com/wavessevaw/GFG-Extreme/blob/a7f424af20cfb56efdbddf3c4e8e4bde414f3c0d/engine/gfg-temporal/src/te_lmc.inc#L113), [engine/gfg-temporal/src/te_lmc.inc:645](https://github.com/wavessevaw/GFG-Extreme/blob/a7f424af20cfb56efdbddf3c4e8e4bde414f3c0d/engine/gfg-temporal/src/te_lmc.inc#L645), [engine/gfg-temporal/src/te_lmc.inc:817](https://github.com/wavessevaw/GFG-Extreme/blob/a7f424af20cfb56efdbddf3c4e8e4bde414f3c0d/engine/gfg-temporal/src/te_lmc.inc#L817).

HYPOTHESIS: 320 КБ/пару обычно заставляют сначала проверять стоимость зависимости/пробуждения/очереди, а не искать потолок пропускной способности. На UMA отсутствие PCIe transfer не устраняет необходимость дождаться GPU. Нельзя приписать весь промежуток между submissions функции fitting.

## Предыдущие подходы

| Подход | Что подтверждено кодом/артефактом | Что остаётся неизвестным |
| --- | --- | --- |
| GFG Open, PR86 | SOURCE VERIFIED: отдельные pyramid/coarse/refine и per-output compose, cached phase commands. Color block matching и sparse candidate screening. | Deck quality/cost относительно той же LSFG модели NOT TESTED. Сам по себе короткий граф не доказал приемлемого качества. |
| Tile TE FULL/LIGHT/REUSE | SOURCE VERIFIED: classification, indirect lists и promotion/fallback. SIMULATED: старый benchmark сравнивает с собственным FULL на lavapipe. | Нет основания переносить −55% на Deck или считать сравнением с LSFG. |
| Motion reuse | SOURCE VERIFIED: tile analysis и LMC analysis уже shared по паре. | Дополнительный cross-pair reuse требует проверки stale motion/scene cuts/occlusion. |
| 2×2 decisions / dual-stage | SIMULATED: имеющийся benchmark включает качество/оценку fetches и отклонённые варианты. | Их физическая стоимость на RDNA 2 и системный выигрыш NOT TESTED. |
| Frame Fusion Core, PR123 | SOURCE VERIFIED: source sharing/scaler placement, не новая математика FG. | Влияние в реально активном game profile NOT TESTED. |
| Native transport, PR125 | SOURCE VERIFIED: native source format и отдельные submitted-work diagnostics. | Число submitted jobs не доказывает завершение/показ; физический выигрыш NOT TESTED. |
| Autopilot/Governor | SOURCE VERIFIED: управляют workload/target/TDP constraints, а не восстанавливают скрытую motion информацию. te.8 сохраняет display target. | Сохранение цели90 само не создаёт90 уникальных показанных кадров. |
| Frame OS / HUD | SOURCE VERIFIED: отдельные pacing/diagnostic Vulkan layers. | Их собственную нагрузку и взаимодействие проверять контрольным прогоном без наложенных лишних слоёв. |

[Имеющиеся te.8 результаты](../docs/TEMPORAL_ENGINE_TE8.md) и [старый tile benchmark](../docs/TEMPORAL_ENGINE_BENCHMARK.md) сохраняются как SIMULATED evidence. В thin-posts и noise-fit есть регрессии; их нельзя скрыть средней выгодой других сцен. Переданные te.7 логи разных игр не дают causal A/B оценки.

## Проверяемые альтернативы до выбора архитектуры

| Эксперимент | Что оставить одинаковым | Условие отклонения |
| --- | --- | --- |
| CPU fit против GPU-only fit | Inputs, exact model/quality, N, resolution, clock/TDP | GPU вариант съедает game headroom либо не улучшает end-to-ready p95 |
| Предыдущая модель / async fit | Timestamp alignment и сцены с acceleration/cuts | Stale model ухудшает local/temporal quality; дешёвый fit не компенсирует |
| Упрощённая global model | Камера + независимые объекты/occlusion | Потеря объектов/границ даже при хорошей средней метрике |
| Pass fusion / fewer barriers | Одинаковые outputs/ownership/validation | Растёт register pressure или GPU span; нет системного выигрыша |
| Transport copy removal | Native format, одинаковый image lifetime | Выигрыш отсутствует либо нарушается semaphore/ownership contract |

Пока ни одна альтернатива не выбрана как заведомо лучшая. Следующий приоритет: одинаковые reference inputs и measured cost decomposition на Deck, затем решение по алгоритму.

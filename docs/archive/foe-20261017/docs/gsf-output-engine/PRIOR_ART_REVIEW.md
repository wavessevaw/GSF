# GSF Frame Output Engine — проверка существующей экспертизы, 2026-10-17

**SOURCE VERIFIED** означает подтверждение в открытых исходниках / зафиксированном
исследовательском пакете, а не в оригинальном Windows LSFG и не на OLED.

## Найденный пакет агента

Исследовательская ветка GFG `codex/native-fg-research` от 2026-10-15
(основа GFG te.8). Перед созданием FOE обязательно читать:

- [research/README.md](https://github.com/wavessevaw/GFG-Extreme/blob/codex/native-fg-research/research/README.md): матрица Gates, явно помечена OPEN.
- [research/LSFG_BINARY_ANALYSIS.md](https://github.com/wavessevaw/GFG-Extreme/blob/codex/native-fg-research/research/LSFG_BINARY_ANALYSIS.md): оригинальный Windows `Lossless.dll` **НЕ** исследован. Нельзя утверждать, что его граф захвата/отрисовки восстановлен.
- [research/MAKO_ARCHITECTURE.md](https://github.com/wavessevaw/GFG-Extreme/blob/codex/native-fg-research/research/MAKO_ARCHITECTURE.md): Vulkan layer+MAKO backend; per-process private Vulkan device, external-memory FD НЕ отдельный IPC-процесс.
- [research/MAKO_FRAME_LIFECYCLE.md](https://github.com/wavessevaw/GFG-Extreme/blob/codex/native-fg-research/research/MAKO_FRAME_LIFECYCLE.md): game present → GPU source copy → prepass → synthetic output → WSI output; реальные кадры тоже передаются через этот транспорт.
- [research/MAKO_SYNC_ANALYSIS.md](https://github.com/wavessevaw/GFG-Extreme/blob/codex/native-fg-research/research/MAKO_SYNC_ANALYSIS.md): общий timeline для источника, private timeline prepass, значения готовности output, fences, resource retirement.
- [research/MAKO_FRAME_PACING.md](https://github.com/wavessevaw/GFG-Extreme/blob/codex/native-fg-research/research/MAKO_FRAME_PACING.md): GeneratedFramePlan (до 4 t), fixed/adaptive admission, smooth cadence отдельно от GPU workload. Готовый кадр можно опоздать передать; просто сдвинуть deadline НЕ значит ускорить генератор.
- [research/MAKO_LIMITATIONS.md](https://github.com/wavessevaw/GFG-Extreme/blob/codex/native-fg-research/research/MAKO_LIMITATIONS.md): не подтверждена Deck end-to-end стоимость / физический scanout.
- [research/NATIVE_FG_ARCHITECTURE.md](https://github.com/wavessevaw/GFG-Extreme/blob/codex/native-fg-research/research/NATIVE_FG_ARCHITECTURE.md): один владелец output, явная модель состояния queued/submitted/GPU ready/WSI handoff/display verified.

## Ключевое отличие оригинального Windows Lossless Scaling

По собственным объявлениям разработчика:
- февраль 2024, Lossless Scaling 2.6: DXGI frame capture использует системный
  захват после представления кадра, альтернативой служил Windows Graphics
  Capture (WGC); обе схемы отличаются от перехвата Vulkan `vkQueuePresentKHR`.
  https://steamcommunity.com/app/993090/discussions/0/4209246056405471570/
- январь/март 2025: LSFG3 Queue Target 0/1/2 — буферизация **входного
  CAPTURE**, не просто +22ms к deadline игрового swapchain. 0 unbuffered,
  1 обычный, 2 для нерегулярных source frames/высокой нагрузки.
  https://steamdb.info/patchnotes/17646233/
- Версия оригинальной DLL, PE imports и приватный output compositor НЕ
  вскрыты агентом. Их нельзя называть BINARY VERIFIED.

**Следствие:** ранее высказанная формула «Windows LSFG работает через
виртуализированный Vulkan игровой swapchain» — НЕ подтверждена и неверно
смешивает Windows-приложение с портом MAKO/Vulkan.

## Решение MAKO, которое уже следует изучить перед новым proxy swapchain

Source-verified в исследовательской ветке: MAKO ведёт source/export
изображения, synthetic outputs и реальный `vkQueuePresentKHR` в одном
Vulkan layer. Новый документ upstream
https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/WSI-ISOLATION.md
прямо говорит о **WSI isolation**: Gamescope остаётся compositor, но
конфликтующий layer `VK_LAYER_FROG_gamescope_wsi` исключается из
обычной managed Vulkan цепочки. MAKO выбирает `OrderedSdr` для FIFO
порядка синтетических + реальных, отдельно gated experimental HDR. Это
source-level design upstream, а не доказательство, что GSF уже совместим
с Gamescope, и не разрешение слепо отключать layer на каждом Deck.

Также MAKO:
- резервирует дополнительные WSI изображения под максимальное N;
- проверяет present flags и pNext, а неподдерживаемые формы ведёт в fallback;
- передаёт real/synthetic FIFO одной транспортной политикой;
- не обязан строить виртуальный game-facing swapchain для генерации.

## Ошибки наших решений

1. LAB.4 сдвигал реальные/синтетические deadline в общем game swapchain,
   но не реализовал входную CAPTURE очередь Lossless Scaling.
2. Прототип FOE (`output_owner_queue.h`) пока чисто контракт/алгоритм:
   не является движком захвата, не имеет real image lifetime и не заменяет WSI.
3. Standalone colored output probe проверяет отдельный Vulkan producer и WSI,
   но вообще не осуществляет захват игры. Это корректный unit/WSI probe,
   а не proof of Lossless capture.
4. Нельзя утверждать, что proxy swapchain — **единственный** возможный
   путь. MAKO демонстрирует альтернативу с WSI isolation и
   синхронизированной передачей в один Vulkan swapchain. Другой путь —
   external compositor capture — потребует определения SteamOS backend
   (на Windows DXGI/WGC неприменимы).
5. В MAKO внешний FD не означает отдельный процесс; не смешивать
   «private GPU image / device» с «independent capture program».

## Decision gate перед следующей реализацией

Сравнить две кандидата **на одинаковом реальном Deck trace**:
A. Проверенный MAKO-подобный единый Vulkan WSI present owner,
   `OrderedSdr`, Gamescope WSI isolation, source copy/timeline,
   admission-before-compute, real-frame priority.
B. Внешний capture/compositor или слой виртуального game swapchain со
   своим output swapchain: проверить реальный доступ к кадрам, backend
   SteamOS, отдельный pixel ownership, latency и sync до обещаний.

Ни один кандидат пока не выиграл по данным. Новый код не должен выходить
как пользовательская LAB до реальной пиксельной проверки порядка и FPS/
frametime оригинальной игры (включая лаг и загрузку), а не счётчика Vulkan.

Все предыдущие опытные сборки остаются экспериментом, стабильный релиз
не менять.

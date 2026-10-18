# Reference reimplementation: проверенные механизмы и открытые пробелы

Основание: [новая директива владельца](https://github.com/wavessevaw/GFG-Extreme/pull/133#issuecomment-6212340968).
Статус: исследовательская спецификация, **не аппаратная приёмка и не выбор готового алгоритма**.
Текущая экспериментальная архитектура сохранена в `e121fca`; релиз и merge отменены.
До доказанного преимущества её дальнейшее расширение остановлено.

## Эталоны и уровень подтверждения

| Эталон | Точная версия / источник | Установка и лицензия | Что подтверждено / чего ещё нет |
|---|---|---|---|
| MAKO Renderer | `render-v4.5.0`, `81d398508592d339da9f4287ea844e695f427630`; просмотренный HEAD `e933b42` имеет идентичные исследованные файлы create/present/pacing/color-policy | Decky ZIP либо Renderer tar.xz, затем installer и `mako-run`/`mako-launch`; GPL-3.0-or-later с исключениями third-party notices | Существуют публичный релиз, исполняемый renderer и исходники транспорта. README требует законную Lossless Scaling установку для FG/LS1. Исходников независимого LSFG-алгоритма этим не получено. Физические 30→90 OLED в этой среде не измерены. |
| lsfg-vk v1 | тег `v1.0.0`, `7113d7d02da9fc9df5cb3b03230d1f7de86f7056`; текущий GitHub HEAD `0af010d` — только миграция на lsfg-vk.dev | Vulkan layer и configuration UI; MIT в root и framegen | Открыт GPU transport и orchestration. Используется LSFG, не независимый публичный аналог всей математики. Старый код — исторический эталон; современная Deck-совместимость не установлена. |
| Decky LSFG-VK Experimental | репозиторий `eugeniosegala/decky-lsfg-vk-experimental`, HEAD `04f427ea853e1d71a3c0e726996974a48504a4f0` | Decky install-from-ZIP; использует установленную Lossless.dll; архивный проект, перенесён в MAKO | Проверены README и существование репозитория. Это установка/управление той же LSFG-семьёй, отдельным независимым алгоритмом его считать нельзя. Лицензии конкретных engine assets требуют отдельной проверки до использования. |
| AMD FidelityFX FSR3 Vulkan swapchain | SDK `v1.1.4`, `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` | SDK для интеграции в приложение; MIT в LICENSE.txt и исследованных исходниках | Открыты реальные Vulkan presenter/interpolation threads, queues, timing, ownership. Код задаёт `numInterpolatedFrames=1`: готовый ×3 этим не подтверждён. Это не готовый универсальный Deck/Proton injector. |
| AMD optical flow / frame interpolation | SDK `v2.3.0`, `60f4ea81909200d8542eca14dccb2628b763a9a3` | Выбранные `Kits/FidelityFX/framegeneration/fsr3` исходники имеют MIT header и перечислены в исключениях `docs/license.md`; общий SDK включает материалы с другой лицензией | Подтверждены исходники optical-flow и disocclusion/inpainting. Нельзя переносить лицензию выбранных файлов на весь SDK. Нет измеренной 3× OLED-интеграции в этом проекте. |

README и release notes — заявления автора продукта. Опубликованный код подтверждает
механизм, а не физическую доставку кадров. Независимого аппаратного сравнения этих
реализаций пока нет. MAKO release notes называют HDR экспериментальным и отдельно
упоминают внешние HDR-дисплеи; это не свидетельство квалификации встроенного OLED HDR.

Источники установки и границ: [MAKO README](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/README.md#install-and-use),
[лицензия MAKO](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/LICENSE.md), [релиз 4.5.0](https://github.com/eugeniosegala/MAKO/releases/tag/render-v4.5.0),
[lsfg-vk README](https://github.com/PancakeTAS/lsfg-vk/blob/7113d7d02da9fc9df5cb3b03230d1f7de86f7056/README.md), [lsfg-vk MIT](https://github.com/PancakeTAS/lsfg-vk/blob/7113d7d02da9fc9df5cb3b03230d1f7de86f7056/LICENSE.md),
[архив Decky](https://github.com/eugeniosegala/decky-lsfg-vk-experimental),
[AMD v1.1.4 MIT](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/LICENSE.txt).

## Проверенный end-to-end путь

| Механизм | Наблюдаемый эталон и исходник | Поведенческое требование для GFG |
|---|---|---|
| Hook и capture | MAKO `engine/mako-render/src/entrypoint.cpp`, `Swapchain::submitSourceCopy`, [real_frames.cpp:277](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/engine/mako-render/src/swapchain/present/real_frames.cpp#L277); lsfg-vk [LsContext::present:130](https://github.com/PancakeTAS/lsfg-vk/blob/7113d7d02da9fc9df5cb3b03230d1f7de86f7056/src/context.cpp#L130) | Принимать фактический acquired image и render-finished waits; GPU-copy в историю. Настоящий present и его ownership не теряются при сбое FG. |
| Motion / interpolation | MAKO выбирает runtime LSFG shaders/models; открытый `generated_frame_plan.hpp` задаёт возрастающие fractions. AMD предоставляет optical-flow, disocclusion и inpainting исходники | Наблюдаемое поведение двух времён 1/3 и 2/3 воспроизводить собственным RGB-only backend либо совместимым открытым компонентом. Не приписывать закрытому LSFG открытую архитектуру или выдуманные weights. |
| План выходов | [GeneratedFramePlan::evenlySpaced:25](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/engine/mako-render/src/generated_frame_plan.hpp#L25), `scheduleAdmittedGeneratedFrames` | Два разрешённых distinct output на одну пару. При частичном допуске честно считать пропуск; не обозначать 2× или duplicate как 3×. |
| GPU synchronization | MAKO `submitSourceCopy`, generated copy waits on acquire и backend timeline; [generated_frames.cpp:403](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/engine/mako-render/src/swapchain/present/generated_frames.cpp#L403) | App waits должны быть поставлены до возврата из Present. Между capture, solver, copies и WSI — действительные GPU dependencies; CPU signal не заменяет producer submit. |
| Разделение потоков / очередей | [AMD Vulkan modes:30](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp#L30), проверка четырёх разных VkQueue [строка 1496](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp#L1496) | CPU worker сам по себе не доказывает независимые GPU очереди. Перед выбором схемы установить возможности очередей RADV и то, что реально provisioned при CreateDevice. Существующий single-queue checkpoint не объявлять эквивалентом AMD scheduler. |
| Pacing | MAKO `OrderedPresentTimeline`, [presentation_policy.hpp:1010](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/engine/mako-render/src/presentation_policy.hpp#L1010), `PresentInvocation::waitForOutput` [present.cpp:36](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/engine/mako-render/src/swapchain/present.cpp#L36); AMD presenter waits between outputs | Каждый выход имеет отдельный срок в единой монотонной шкале. Отдельно учитывать готовность GPU, глубину compositor queue и display period. Само получение свободного VkImage не задаёт время показа. |
| MAILBOX/FIFO | [gamescopeScalingPresentContract:21](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/engine/mako-render/src/swapchain/create_policy.hpp#L21) | MAKO при подтверждённых условиях использует **lower MAILBOX + compositor FIFO**, не глобальную замену всех MAILBOX на FIFO. Каждому private bridge output нужен отдельный future deadline. Сначала подтвердить слой, поверхность и timing clock. |
| HDR | MAKO [frame_color_policy.hpp:10](https://github.com/eugeniosegala/MAKO/blob/81d398508592d339da9f4287ea844e695f427630/engine/mako-backend/src/helpers/frame_color_policy.hpp#L10), `selectPackedHdr10Transport` в resources.cpp | Различать PQ и linear scRGB. Full-quality HDR требует корректного transfer-function пути. MAKO прямо маркирует интерполяцию PQ code values как reduced/approximate. FP16 сам по себе не делает PQ interpolation физически корректной. |
| Metadata / lifetime | MAKO `FilteredPresentPNextChain`, `swapchain/retirement.hpp`, per-image present fences в create.cpp | Caller-immutable chains; ID/fence принадлежат нужному настоящему кадру; вывод остановлен и GPU/WSI uses закончены до уничтожения ресурсов. Непонятный prefix не копировать с угаданным размером. |
| Gamescope → display | [STEAMOS_DEEP_AUDIT.md](STEAMOS_DEEP_AUDIT.md): WSI protocol → commit selection → compositor/direct scanout → DRM `page_flip_handler` | Связать R/G1/G2 с Gamescope commit и DRM framebuffer/page-flip. WSI calls и GPU content hashes отдельно от физических кадров. При возможности подтвердить смену пикселей оптически. |

## Почему AMD не подключается как готовая замена

Исследованный Vulkan presenter SDK v1.1.4 [строка 2036](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp#L2036)
запрашивает один intermediate. В новом SDK `FfxFrameInterpolationDispatchDescription`
содержит optical-flow, dilated depth, dilated motion vectors, reconstructed previous
depth, camera parameters и transfer function. См. SDK v2.3.0
`Kits/FidelityFX/framegeneration/fsr3/include/ffx_frameinterpolation.h:315`.
Обычный Vulkan present hook этих игровых данных не предоставляет. Нужно проверить
поддерживаемый RGB-only поднабор; подставлять выдуманные depth/MV и объявлять
полный FSR3 нельзя. AMD presenter полезен как открытый эталон synchronization и
pacing, optical-flow — как кандидат с проверяемой лицензией и стоимостью.

## Разрыв текущего checkpoint с эталонами

1. **WSI layer order неизвестен.** Последний лог показывает MAILBOX/HDR и реальный bypass.
   Он не устанавливает, видит ли GFG исходный запрос игры либо внутренний MAILBOX
   Gamescope. Сохранённый FIFO override проверен только на XCB software surface.
   Его нельзя рекомендовать как исправление Gamescope до проверки этого различия.
2. **Нет compositor deadline bridge.** Бounded CPU worker и FIFO handoffs не
   воспроизводят MAKO lower/compositor contract с отдельным future deadline.
3. **Единая GPU очередь.** Host mutex обеспечивает допустимые обращения, но не
   предоставляет очередь вывода, независимую от очереди игры. Выбор расширенной
   топологии требует reference-backed capability plan до изменения CreateDevice.
4. **Алгоритмический паритет не установлен.** LMC/tiles — сохранённые кандидаты.
   Нет одинакового набора входов, измеренного output oracle и динамической оценки
   MAKO/GFG на Deck. Подсчёт строк кода не выбирает лучший backend.
5. **HDR precision ≠ HDR quality.** Проверка 1024 уровней подтверждает статичный
   packed10 roundtrip. Цветовой и временной паритет с quality HDR не установлен;
   смешивание encoded PQ в checkpoint остаётся approximation.
6. **Scanout evidence отсутствует.** Ни внутренний counter, ни present-wait
   не закрывают требование 90 физических уникальных кадров.

## Минимальный вертикальный срез и порядок работ

1. Зафиксировать baseline реального эталона: законная локальная установка MAKO,
   точные engine/model версии, 1280×800/90Hz, одна детерминированная сцена 30 FPS.
   Использовать уже существующий `tools/native_fg_probe.py` только как image/quality
   smoke, а не как измерение scanout или throughput. Закрытые payloads не включать
   в собственный runtime, исходники или артефакт.
2. Воспроизвести проверенный транспорт на одной подтверждённой WSI topology:
   идентифицировать позицию hook и Gamescope protocol; согласовать владельца
   deadline/FIFO; подать маркированные GPU R/G1/G2. Сначала доказать порядок,
   уникальность, 90Hz delivery, bounded latency и disable/rollback на Deck.
   Детерминированный marker transport — инфраструктурная проверка, ещё не FG-продукт.
3. Подменить только interpolation backend собственным implementation по проверенной
   RGB-only спецификации либо разрешённым open-source компонентом. Сохранить те же
   входы и сроки; сравнить occlusion, HUD, cuts и GPU cost с эталоном. Две дробные
   фазы должны быть реально рассчитаны, а не повторены или повторно размечены.
4. Расширять сцены/форматы после минимального slice: камера, тонкие объекты,
   particles, disocclusion, HUD, HDR. Любое расширение должно устранять измеренный
   gap и не ухудшать 30 source FPS. Затем 10 минут динамики / 30 минут стабильности,
   power, temperature, latency и физический output. До этого без релиза/merge.

Для собственных модулей сначала фиксировать наблюдаемое поведение и тесты, затем
писать собственную структуру и код. Механическое переименование чужих исходников
независимой реализацией не считается. Разрешённые reused components остаются явно
атрибутированными и с соответствующими лицензиями.

## Препятствия и продолжение

- Авторизованного аппаратного endpoint в среде нет; uploaded device inventory
  его не создаёт. Следовательно, аппаратный baseline и физическая приёмка открыты.
- Повторная проверка официальных SteamOS recovery/package hosts 16.10.2026
  снова дала CONNECT 403, 0 bytes. Индекс и recovery image не получены; SHA образа
  отсутствует. Recovery 3.8.14 не выдаётся за установленную SteamOS 3.8.28.
  См. `evidence/steam-source-probe-20261016-repeat.txt`.
- Native CI checkpoint `e121fca` [run 38895664221](https://github.com/wavessevaw/GFG-Extreme/actions/runs/38895664221)
  **FAILED** в шаге bounded async output. Причина именно удалённого падения не
  извлечена из authenticated logs; локальный old-validation hazard описан отдельно.
  Ничего не удалять из обязательных проверок ради зелёного статуса.
- Локальная свежая validation после shader padding: cold run 8 real / 12 synthetic /
  2 skipped, zero validation errors; warm run 8/14/0, zero validation errors.
  Cold deadline loss остаётся видимым, software timing не является Deck timing.

Следующий выбор реализации зависит от наблюдаемого reference transport и
проверки RGB-only алгоритмического кандидата. Не возвращаться к расширению
экспериментального FG3 по предположению, что один CPU worker уже решает display.

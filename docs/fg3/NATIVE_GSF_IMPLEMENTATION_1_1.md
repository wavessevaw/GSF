# Native GSF: implementation checkpoint 1.1

17.10.2026 (Asia/Vladivostok). Исходный audit: PR133 `5782aa9`,
инвентаризация `31d5e85`, первый implementation checkpoint `577789f`.
Основная архитектура ADR1.0 сохранена; новые сравнения алгоритмов не запускались.

## Реализованный путь

Game QueuePresent entry фиксирует CLOCK_MONOTONIC до инициализации и backpressure.
Worker сохраняет capture/admission times, не удерживает caller pointers, выполняет
одно задание и хранит максимум одно следующее. Переполнение создаёт backpressure,
а не неограниченную очередь. Перед возвратом приложению его binary waits уже
submitted в private per-image gate. GPU capture, source history, pyramid,
shared motion/model/visibility и реконструкция выполняются без CPU pixel staging.
Поколение bridge и последовательный source/pair ID не переиспользуются внутри
swapchain. Destroy/recreate останавливает worker и дренирует GPU до освобождения.

G1/G2 проходят через собственные swapchain images, acquire semaphores, GPU
copy/blit и обычный нижний QueuePresent. Original real image сохраняет fences,
IDs и timing namespace приложения. Неизвестная цепочка metadata: drain и real-only.
Новый `te_output_control` читает только coherent control header результата после
его timeline value, вместо `te_context_idle` всей пары. CPU pixels не читаются.

GPU shared analysis остаётся общим. ×2 запрашивает один выход в 1/2; ×3 — 1/3 и
2/3. Планировщик может допустить только G2: тогда вычисляется один результат в
2/3, а не два результата с одним потерянным. Timeline всё равно завершает блок V+2;
следующая source copy не ждёт никогда не отправленного значения. Поздний G1 не
отменяет G2. Ошибка транспорта отменяет оставшиеся выходы и сохраняет безопасный
путь real/error/recreate. Cut и точный RGB duplicate не засчитываются как synthesis.

## Время и задержка

Для capture timestamps C0,C1, dt=C1−C0:

| Content | Позиция | Целевое время представления |
|---|---|---|
| G1 | C0+dt/3 | C1+dt/3 |
| G2 | C0+2dt/3 | C1+2dt/3 |
| R1 | C1 | C1+dt |

Нужен R1: задержка content относительно source timestamps — один интервал dt,
около33.3мс при30realFPS, плюс реальный compositor/display path. Это не измерение
input-to-photon. Сроки не пересчитываются от позднего старта worker. Просроченный
slot выбрасывается. Переменный dt меняет позиции; interval discontinuity/долгая
пауза сбрасывает интерполяцию. Слоты ближе refresh period подавляются; при изменении
FPS фаза real ограничена C1+2dt и затем re-anchor. Нет бесконечного phase accumulator.

При включённом private GOOGLE timing WSI submit делается за четверть refresh
period до desired time. Без расширения используется host handoff в target time.
Это начальная транспортная политика; её оптимальность на Gamescope не доказана.
Готовность проверяется отдельно от target, skip G1/G2 независим.

## Gamescope

Выбран порядок **GFG → FROG Gamescope WSI → ICD**. GFG видит public FIFO и каждый
свой G1/G2/R передаёт FROG. FROG может использовать driver MAILBOX, сохраняя
compositor FIFO. `GFG_NATIVE_FORCE_FIFO=0`: нельзя подменять нижний MAILBOX вслепую.
Генерируемый GFG launcher выбирает guarded manifest или системный manifest;
standalone launcher использует обнаруженный manifest. Выбор зависит от активного
Gamescope session. Без подходящего FIFO-контракта остаётся real-only.

`VK_GOOGLE_display_timing` включается только если extension объявлено устройством.
GDPA pointer не считается разрешением. Когда namespace принадлежит приложению,
его timing chain/feedback не перехватывается. В собственном namespace каждый
G1/G2/R получает отдельный ID и desiredPresentTime. Доступные refresh-cycle queries
обновляют clock, если пользователь не задал OUTPUT_HZ. Feedback читается ограниченно
и маркируется как compositor/API evidence. Он не доказывает optical scanout.

Проверены исходники Gamescope (pinned commit в STEAMOS_DEEP_AUDIT), packet-builder
и namespace fallback. **В текущей среде нет Gamescope/DRM/Deck, runtime-контракт
на целевой платформе NOT TESTED.** Ни вызов present, ни SUCCESS не доказывает экран.

## Интеграция и наблюдаемость

Отдельный native module, RPC и UI Off/×2/×3. Настройка применяется при перезапуске;
она не меняет power/TDP/частоту/лимиты Governor. При выборе native MAKO отключается
для запуска, включая случай отсутствующего компонента. Flatpak FG не принимается;
он сохраняет real-only. Встроенный installer проверяет SHA-256, запускается от
владельца game home и сохраняет rollback. Marker читается игровым пользователем,
даже если Decky backend работает root. Обычный launcher и dev profile выбирают GSF.

Status snapshot максимум4Hz; PID/start ticks проверяются, retired context удаляется.
Подробный pairs.jsonl включается diagnose.sh и содержит generation/pair/source IDs,
capture/admission/worker timestamps, planned/compute/copy masks, GPU completion
samples, readiness observations, сроки, WSI acceptance, drop classification и
отдельные GOOGLE IDs/feedback. Визуальная уникальность/physical FPS — null.
accept_report.py выводит измеренные source/WSI rates, не Real×3. Bootstrap без R0
не включается в потери действительных пар. GPU profiling — выборка, не полный trace.

## Что подтверждено и что устранено

- 17 software pipeline checks: 183 core assertions, borrowed-device lifetime,
  sync real-only fallback, pixels/order/spacing ×2/×3, cut, duplicate, recreate,
  queue idle, unknown metadata, validation выше/ниже GFG. Cold startup и отсутствие
  catch-up явно учтены. Старое утверждение «source3Hz» исправлено: старый fixture
  подавал source30Hz; теперь источник задаётся отдельно.
- Native contracts проверяют bounded worker, caller lifetime, semaphore consumption,
  ring failure/reclaim, source clocks, переменный dt/refresh, namespaces/packet times.
- Backend regression и browser smoke проверяют выбор native mode и restart feedback.
- В low-refresh тесте source4Hz/output9Hz прошло5 G2-only dispatch/presents без VVL
  ошибок: один compute output действительно соответствует логической позиции G2.
- ABI blocker: обычная host-сборка требовала glibc2.38. Production ELF пересобран
  через glibc2.31 SDK; максимум символов2.17, успешный dlopen под реальным loader2.31.

## Причины пропусков 90Hz — границы данных

96×64 lavapipe, задан source30Hz, без VVL,45 source submits. В холодном запуске
~1.2с создания pipeline; source interval и полный capture span поэтому различаются.
Это не аппаратная производительность и не сравнительный benchmark.

| Категория | Наблюдение | Статус |
|---|---|---|
| A compute | После исправлений first-ready median18.74ms, last19.43ms, p95~35ms; первый budget~11.1ms | Подтверждено для LVP, не RDNA2 |
| B sync | Убран all-pair control wait; копияG1 на одной GPU queue всё ещё после computeG2 | CPU wait исправлен; queue provisioning остаётся |
| C timing | Startup/capture age раньше терялись; сейчас median capture→worker0.20ms, p95~20.89ms, max~1.2с | Источник и backpressure различимы |
| D WSI | Есть отдельные WSI acquire/accepted timings; Gamescope feedback в cloud отсутствует | Целевой compositor NOT TESTED |
| E game contention | Fixture — движущийся pattern, не игра | NOT TESTED |
| F drop rule | 26 раз G1 пропущен, G2 принят; прежний break потерял быG2 | Исправлено и воспроизведено |

45 real/28 generated WSI acceptances не превращаются в90 FPS. Полный accepted span
даёт~44.99API handoffs/s; физическая частота неизвестна. Изменение цены невостребованных
выходов проверено функционально; статистического speedup claim из коротких запусков нет.

## Остающиеся барьеры и выпуск

C hardware acceptance: **NOT TESTED**. Нет /dev/dri и подключённого Deck endpoint.
Невозможно проверить SteamOS Gamescope/DRM/optical scanout через software Vulkan.
D practical quality/power/latency/stability: **NOT TESTED**. Scene-cut heuristic
пропускал renderer cut; HDR PQ интерполируется в code space; широкое игровое качество
не принято. Новый PSNR sweep не запускался, пока compositor/hardware не проверены.

Экспериментальные устанавливаемые ZIP допустимы для следующего этапа. Stable release
и заявление «30→90 на OLED выполнено» запрещены до C/D. Сохраняются исходники,
точные доказательства, пакет и аппаратный сценарий; поручение остаётся открытым.

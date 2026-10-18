# План аппаратного сравнения

DEVICE VERIFIED: получен пользовательский inventory SteamOS3.8.28 / Galileo / RADV Mesa26.0.0-devel, [сводка](results/device_inventory_20261015.json). NOT TESTED: генерация, сравнительная стоимость, качество и физический показ.

До каждого прогона: source/patch/binary/DLL SHA, SteamOS/Mesa/RADV, модель OLED/LCD, игра/сцена/маршрут, render/output resolution, TDP, refresh/VRR, питание, clock policy, температура. Cold start и warmed steady-state сохранять отдельно. Не менять одновременно алгоритм, scale, режим модели и pacing.

| Режим | Нормализованные t | Назначение |
| --- | --- | --- |
| 30→60 | 1/2 | Базовый2× |
| 30→90 | 1/3,2/3 | Базовый3× |
| 45→90 | 1/2 | Другая нагрузка real frames |
| 20→60 | 1/3,2/3 | Ограниченный game budget |
| 18→90 | 1/5,2/5,3/5,4/5 | Стресс; не обязательная функция первой версии |

Два вида испытаний: одинаковые saved inputs для алгоритмического качества; реальная игра для full-system cost. Стенд original LSFG на Windows отдельно от MAKO+LSFG на Deck, без прямого ранжирования ОС/GPU по голой цифре.

A/B/A: A=игра без FG, B=выбранный единственный backend, A=без FG; дополнительно MAKO+LSFG↔candidate на одинаковых настройках. В старых GFG UI `Off` означает выключение экспериментального TE и может оставлять обычный LSFG: это НЕ no-generation baseline. Регистрировать фактический backend и multiplication=0/отсутствие generation, а не только имя переключателя.

Исходная серия: повторить одинаковый маршрут после прогрева, чередовать порядок backend; сохранять сырые интервалы и timestamps, p50/p95/p99, real FPS, distinct outputs, queue/admission drops, power/clock/VRAM/temp. Короткий probe/reference smoke недостаточен для tails и мощности; p99 на четырёх synthetic outputs не рассчитывать как репрезентативный.

Quality: FLIP/PSNR/SSIM на одинаковом t, LPIPS при зафиксированной зависимости/весах, local crops тонких объектов/HUD/occlusion, temporal/flicker и просмотр кадров/видео. Средние метрики не маскируют систематический текстовый артефакт. Если метрика не посчитана, поле отсутствует/NOT TESTED, а не ноль.

Latency: ready span — engine readiness, QueuePresent — handoff. Для input-to-display/физического cadence нужна подтверждённая display feedback или независимое high-speed наблюдение с event markers. Не выдавать GPU timeline completion за показ пользователю.

Критерии продолжения: independent startup/failure fallback работает; сопоставимые данные позволяют проверить2мс отдельно от analysis; реальный FPS защищён; снижение total cost/энергии подтверждено; качество сопоставимо с заранее выбранным reference profile. При недостижении цели сохранять причины и открытый gate.

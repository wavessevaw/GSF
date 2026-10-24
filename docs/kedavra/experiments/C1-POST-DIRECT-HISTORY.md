# C1: экспериментальная сборка post → history

Выбрана стратегия C1 из [Hardware-Native Runtime research](https://github.com/wavessevaw/GSF_Kedavra/pull/78):
последний проход фильтров пишет сразу в history image. Отдельный полноэкранный
`RecordCopy(post → history)` не записывается. Алгоритм генерации, фильтры, motion analysis,
очереди, KMS и управление мощностью не меняются.

База — Kedavra 1.4.1, `5f22938b55f9853f764e927c847d9c1a8d9a8c04`.
Это **тестовый бинарник**, не официальный релиз. Его исходный head и фактически собранный
Actions merge commit указаны в BUILD-INFO.json и KEDAVRA/EXPERIMENT.json внутри ZIP.
Версия основной панели остаётся 1.4.1; ориентироваться на manifest и status, не только номер UI.

## Область применения

Только генерация с активными post filters, SDR и совместимым отдельным history target.
Проверяются размер, формат, принадлежность slot и физическое отсутствие aliasing со всеми
inputs/scratch images плана. Переназначается только destination последнего pass;
промежуточные ping-pong images и shader constants остаются прежними.

HDR/PQ, scRGB, post-only без generation, фильтры Off и любой неприменимый target используют
существующий путь. DirectScan в эту сборку не входит. Для игры без фильтров выигрыша от C1 нет.

## Сборка и проверки

GitHub Actions workflow KEDAVRA собирает standalone shaders/tests, Gamescope на закреплённой
базе `aa289ea5977d8b21b92b0f246f0e8f0c225d180f`, runtime со своими libraries и Decky ZIP.
Для этой experimental PR-ветки дополнительно выдаётся артефакт `KEDAVRA-C1-test-<head SHA>`
с manifest, checksum и этими инструкциями. Release job для PR не запускается.

В `kfg_posttest` добавлены проверки безопасного переназначения и **точного совпадения всех
RGBA8 пикселей**: обычная цепочка + identity image copy в history против direct destination.
Проверяются все отдельные эффекты, CAS/DLS/FXAA/SMAA, все presets, три последовательных
кадра с различными phase/alpha и размерами 320×200 и 63×47. Последний размер проверяет
неполные workgroups. Такой тест проверяет texel parity; vkCmdCopyImage в тесте не является
моделью времени compute-copy Gamescope.

Локальное воспроизведение на Linux:

```sh
make -C kedavra all
make -C kedavra test-pacing test-sched test-engine
```

Для software correctness выбрать lavapipe ICD; для Deck bench — RADV и сохранить device/driver.
Прохождение lavapipe не подтверждает аппаратное ускорение или правильный физический вывод.

## Установка и управление из плагина

Концепция продукта: установка runtime, настройка, диагностика и откат доступны в плагине.
Пользователю не нужны терминал, convars или редактирование файлов. ZIP плагина устанавливается
через стандартный интерфейс Decky; затем все действия выполняются в GSF Kedavra:

1. **Setup → Install / Reinstall C1 test runtime → Reboot now**.
2. **Setup → Image filters**: выбрать тот же пресет для всех сравнений (например Sharp/CAS).
   Есть Off, Sharp, Vivid, HDR look (эффект SDR, не переключение HDR), Cinema, Noir, Retro, Smooth.
3. Включить Auto или Custom на главной панели; для тестов выбрать нужный base FPS.
4. **Setup → C1 direct history**: Off — A, On — B, снова Off — A.
5. **Pace** записывает trace; **Setup → GPU benchmark / Save report to Desktop** собирают диагностику.

По умолчанию C1 **выключен**, фильтры **Off**. Настройки сохраняются плагином и восстанавливаются
после перезапуска Gamescope. Включение C1 отклоняется при старом или устаревшем status runtime;
панель предлагает Reinstall и Reboot. Ошибка команды не сохраняет новое значение; backend
пытается восстановить прежние параметры. Панель показывает сохранённый запрос и подтверждённый
runtime flag отдельно, счётчики записанных direct/copy команд и отсутствие измеренного выигрыша.
Не следует трактовать On как доказательство выполнения C1 на каждом кадре.

Для разработчика backend использует `kfg_post` и `kfg_post_direct_history` через gamescopectl,
но пользователь работает только с UI. Диагностические поля свежего status:

- `experimental_build == "c1-post-history-v1"`;
- `post_direct_history_requested` отражает переключатель;
- `post_direct_history_sdr_records` растёт в применимом режиме B;
- `post_history_copy_records` растёт в A или при copy fallback.

Это счётчики **записанных команд**, не доказательство scanout или новых пикселей.
Их не нужно сбрасывать: сравнивать дельты за выбранный интервал. Смена флага не очищает
историю и не меняет policy/filter spec. Для полезного теста выбрать неизменный SDR профиль
с фильтром, например уже используемый CAS; не менять фильтры одновременно с A/B.

На отдельном тестовом Deck: 45→90 ×2, 30→90 ×3 и 30→60 ×2; дополнительно 24→90.
После прогрева сравнить A/B/A по одинаковому игровому маршруту, настройкам, яркости,
Mura/HUD, power policy. Собирать real FPS, GPU elapsed, KMS trace, content/optical checks,
APU power и длительные pacing tails. Кратковременные значения status недостаточны.
Численный выигрыш пока **не измерен**.

## Установка и откат

Эта задача только создаёт сборку: текущая установка пользователя не меняется автоматически.
ZIP имеет тот же Decky plugin ID KEDAVRA и при ручной установке заменяет существующую копию.
Перед такой установкой сохранить рабочий ZIP/настройки и проверить путь возврата к нему.
Не устанавливать modified Mesa/kernel и не изменять firmware/SMU или power/thermal limits.

Быстрый rollback C1 — **Setup → C1 direct history → Off**. Возврат к системному Gamescope —
**Setup → Remove and restore stock Gamescope → Reboot now**. Возврат к сохранённому стабильному
плагину — через стандартный интерфейс Decky; затем его **Setup → Install / Reinstall → Reboot now**.
Не удалять/подменять файлы активной Game Mode session.
При чёрном экране, GPU reset, corruption или ухудшении real FPS прекратить аппаратный тест.

Исследование/план следующего этапа — [DirectScan](https://github.com/wavessevaw/GSF_Kedavra/blob/967481efe51dbb3813a5b11fb3fdb97ffb1204d7/docs/kedavra/research/hardware-native-runtime/02-DIRECTSCAN.md).
В main этого пути пока нет; research-документы доступны в отдельном PR #78.

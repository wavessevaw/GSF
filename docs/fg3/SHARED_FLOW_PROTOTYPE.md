# GFG Shared Flow — вычислительный прототип

Основное решение: [Architecture Decision 1.0](ARCHITECTURE_DECISION_1_0.md).
GSF (`te-bench --algo gsf`) использует coarse-to-fine двунаправленное движение,
один robust affine candidate, сохранённую dominant-motion palette, dense visibility
и bounded orphan refinement. Начиная с `26416e2`, несогласованный affine predictor
включает полный projective fit на GPU. Общий анализ пары выполняется один раз; выходы
`t=1/3` и `t=2/3` реконструируются отдельно. Это offscreen prototype, не prerelease.
Исследовательская фаза закрыта. Начиная с `de48b0d`, выбранный backend подключён
к native layer через `GFG_NATIVE_ALGO=gsf`; coherent opt-in профиль задаёт
`scripts/run_gfg_gsf.sh` (GPU bridge, async, same-device, 90 Hz host clock).
Это инженерная интеграция полного пути, не публикация готового продукта.

## Что проверяется

- Одни и те же input arrays и held-out targets для GPU LMC tier0 и GSF.
- Synthetic reference: собственный renderer, 90 Hz, stride3, 1280×800.
  Отдельные окна: strafe, objects, fast turn, HUD update, post-cut, crossing cut.
  Первые пять окон по 40 кадров, последнее — 11. Это не запись коммерческой игры.
- Реальные camera clips Intel IoT DevKit `sample-videos`, CC-BY-4.0:
  `bolt-multi-size-detection.mp4`, `worker-zone-detection.mp4`.
  Их native rate — 60000/1001 Hz; stride3 означает 19.98→59.94, не 30→90.
  Окна от 5 и 10 секунд, decode/scale Lanczos в 384×216, 31 и 61 кадр.
- Quality run: GPU execution с Vulkan validation и download, PSNR-Y/SSIM-Y,
  temporal error, motion ROI, moving-edge ROI, HUD rectangles, endpoint duplicates.
  Motion ROI берётся из разницы endpoint/target >5 RGB levels; это только oracle
  для оценки качества, не скрытая информация для runtime алгоритма.
- Sharing ablation: тот же source stride/history, но только один выход.
  Первый выход должен совпасть побитно с первым выходом FG3 на каждой паре.
  Это проверяет корректность переиспользования анализа, не скорость дисплея.
- Timing: отдельные `--no-download` запуски, порядок методов меняется между
  повторениями. Из каждого контекста убирается только первая пара; raw первая
  пара сохраняется. Median/p95 берутся из оставшихся пар, не из средних проходов.
  Измеряются analysis, S1, S2, first/pair-ready GPU timestamp spans, CPU submit/wait.
- Upload GPU copy расположен до analysis timestamp; CPU copies и output readback
  не входят в no-download compute interval. Поэтому это не полная end-to-end цена.
  Pair-ready включает межпроходные/межвыходные queue gaps и малую telemetry copy.
  Сумма медиан отдельных проходов не обязана равняться медиане общего span.
- `allocation_bytes` — сумма реально запрошенных Vulkan allocations контекста,
  включая host staging. Это не измерение полного VRAM residency: driver/pipelines,
  игра и Gamescope сюда не входят. У выбранного GSF память не меньше GPU LMC.

## Отрицательные результаты и пересмотр

| Контрольное упрощение | Наблюдение | Вывод |
|---|---|---|
| Compact block field / собственная компактная реконструкция | bolt −2.207 dB; synthetic small strafe −1.188 dB; видимое раздвоение малого объекта | Не заменяет dense visibility |
| Dense visibility без fit и palette | small post-cut −0.492 dB | Удаление всей глобальной информации не обосновано |
| Dense + один affine, без palette | native strafe −1.670 dB | Малые сцены скрывали полезность global hypotheses; нельзя квалифицировать 1280×800 по 384 px |
| Dense + affine + palette (выбранный GSF) | первый native probe 32.093 dB против 31.914 у LMC | Palette возвращена; нужны остальные сцены и cost gates |

Предыдущий `scene cut` segment начинался **после** cut и не проверял сам переход.
В новом корпусе crossing cut выделен отдельно. Дубликаты при recovery не считаются
двумя успешными synthetic кадрами. Detector — эвристика, а не гарантия обнаружения
любых монтажных склеек; ложные срабатывания и пропуски требуют отдельного учёта.

Новый тест независимого noise cut сначала провалился: coarse luma сгладила шум,
поэтому порог её matching cost не обнаружил смену сцены. Исправление считает
полноразмерный RGB residual после компенсации движения блока. Cut vote: более
75% блоков имеют mean RGB residual >0.15. На cut выводятся ближайшие endpoints;
предыдущий cut запрещает temporal predictor следующей пары. No-cut bit-exact
и выход из recovery проверяются тестом, не предполагаются.

Fresh VVL также выявил ArrayStride на Function array `seen[4]`, когда shader стал
читать globals. glslang 15.2.0 объединил его тип с SSBO palette[4]. Локальный массив
расширен до пяти элементов (используются четыре), чтобы разделить типы. Validation
не отключалась. Ранее использован такой же обход для двух Workgroup arrays.

## Воспроизведение

Нужны Vulkan 1.2 compute/timeline, glslangValidator, spirv-val, C11 compiler,
make, ffmpeg/ffprobe; Python numpy, Pillow, moderngl, scikit-image. Для software
сравнения выбрать Mesa lavapipe ICD и Khronos validation; при сравнении методов
не запускать параллельно renderer, сборку и другие CPU/GPU workloads.

```sh
make -C engine/gfg-temporal all lib
python3 tools/temporal_bench/prepare_shared_flow_corpus.py --out /tmp/gsf-corpus
python3 tools/temporal_bench/compare_shared_flow.py \
  --bench engine/gfg-temporal/build/te-bench \
  --sequence /tmp/gsf-corpus/target-strafe \
  --sequence /tmp/gsf-corpus/target-objects \
  --sequence /tmp/gsf-corpus/target-fastturn \
  --sequence /tmp/gsf-corpus/target-hud \
  --sequence /tmp/gsf-corpus/target-postcut \
  --sequence /tmp/gsf-corpus/target-crosscut \
  --sequence /tmp/gsf-corpus/bolt-small \
  --sequence /tmp/gsf-corpus/worker-small \
  --algorithms lmc-gpu,shared-flow-palette --pairs 12 --timing-repeats 3 \
  --check-sharing --work /tmp/gsf-results
GFG_TEST_SHARED_FLOW=4 engine/gfg-temporal/build/te-tests
```

Эта команда воспроизводит закрытый variant3; текущая native система использует variant4.
Camera revision и SHA-256 clips/license зафиксированы в preparer. Manifest сохраняет
полные hashes и metadata каждого input; comparison — commit, code diff hash,
binary hash, input/output hashes и raw command logs. `--reuse` использует имеющиеся
arrays и заново сохраняет их hashes. Renderer зависит от Mesa/font versions:
другой драйвер не обязан дать побитно тот же корпус; оба метода обязаны получать
**одинаковые локальные arrays**. В repository не включаются большие videos/NPY.

## Границы результата

Cloud device — llvmpipe (LLVM 19.1.7), четыре CPU worker threads. Это не RADV APU
Steam Deck. Даже уменьшение software timestamp span не доказывает экономию Deck
power или достижение p95 ≤6 ms. Первый shader JIT тоже сохранён отдельно:
первоначальный compact cold probe занимал около 17 секунд; steady-state timing
не описывает стартовую готовность.

Латентность двукадровой интерполяции требует будущего настоящего кадра: около
одного source interval lookahead (33.333 ms при 30 FPS) плюс readiness и display
margin. Здесь измеряется вычислительная готовность; input-to-photon, contention
с игрой, HDR/PQ dynamic quality и physical 90 Hz не измерены. Новый prerelease
не публикуется, пока аппаратная приёмка отсутствует.

## Состояние проверки

Все сравнения закрыты и сохранены в [архиве](evidence/gsf-research-20261016.zip).
Читаемые summaries: [compact/dense](evidence/gsf-compact-dense-comparison.json),
[affine](evidence/gsf-affine-comparison.json),
[fixed model с palette](evidence/gsf-fixed-model-comparison.json).
В последнем raw `gsf` означает variant3 на `75ca946`; текущий `gsf` — variant4
с confidence-triggered projective path. Не смешивать результаты этих версий.

Закрытое полное сравнение variant3 при 1280×800: strafe +0.154 dB / GPU span ratio
0.806, objects −0.116 / 0.866, fastturn −0.426 / 0.953, HUD −0.101 / 0.830,
post-cut −0.008 / 0.858. Post-cut HUD rectangle потерял 2.007 dB средней frame-PSNR;
этот gate не пройден, даже при малой общей разнице. Crossing cut detector пропустил.
Реальный worker clip не лучше nearest endpoint; хорошие PSNR к LMC это не скрывают.
Компактный путь не выбран, 15% gate на всех сценах не пройден и не понижен.

Последний выполненный probe до закрытия исследований: adaptive variant4 на том же
fastturn получил 23.361 dB против 23.364 LMC; projective path включался на 9/12 пар.
Новая серия PSNR comparisons не запускается. Архитектура принята с этими рисками;
по P0 директиве работа продолжилась интеграцией, а не поиском идеальной метрики.

## Результат реализации полного native пути

Код `de48b0d`, harness/исправление тестового приложения `7108ce5`:
capture → app-device GSF → отдельные G1/G2 → bounded worker → paced G1/G2/R → WSI.
Реализованы GPU readiness waits в worker, deadlines и пропуски опоздавшей генерации,
lookahead, late-source resync и stale budget, зависящий от периода источника.
В синхронном metadata fallback pacing отключён. Исправлен расчёт percentile:
для двух samples p95 раньше мог оказаться ниже median.

Функциональная приёмка: [acceptance.json](evidence/gsf-native-acceptance.json).
Тринадцать checks прошли: core GSF (182 assertions), borrowed-device pixels/lifetime,
фактические G1/G2/R pixels и spacing, recreate, queue idle и unknown metadata —
в обоих порядках GFG/validation; дополнительно cut recovery без synthetic credit
в обоих порядках и real-only fallback без worker. Native Python regression: 31 tests passed.
CPU/GPU LMC baseline: по 168 checks, 0 failures.

Software clock установлен **9 Hz** для функциональной приёмки под fresh VVL:
4 real + 6 distinct synthetic, content/order/spacing PASS, минимум handoff interval
около 111 ms. Это 3× pipeline при source3/output9, не доказательство 30→90.
Выполненные проверки с budget90 на lavapipe пропускали generation и сохранены
как отрицательные результаты. Drop не переименовывается в successful synthetic.

Первый автоматизированный двухпорядковый прогон обнаружил VUID-00067 в тестовом
приложении: один render-finished semaphore переиспользовался для разных WSI images.
Render fence не доказывает завершение presentation wait. Harness исправлен на
per-image ready semaphores; ни validator, ни error matching не отключались.
После исправления оба порядка прошли без validation errors. Это исправление
тестового приложения не объявляется доказательством всех synchronization контрактов Deck.

`1151078` устраняет отдельную runtime ошибку учёта: cut endpoint copies больше
не считаются synthetic. Перед WSI admission worker получает GPU control header
без чтения пикселей. Чёрно-белые scene cuts проверены end-to-end: 4 real,
0 synthetic, 6 skipped_cut. `852b7ca` сохраняет real-only путь при отсутствии
worker. При этом detector всё ещё пропускает сложный renderer cut, указанный выше.

Воспроизведение системной проверки после сборки core/native/observer targets:

```sh
xvfb-run -a python3 scripts/check_gsf_pipeline.py \
  --native-build engine/gfg-native/build-te \
  --core-build engine/gfg-temporal/build \
  --validation-layers /usr/share/vulkan/explicit_layer.d \
  --out /tmp/gsf-pipeline-results
```

CI запускает этот функциональный этап и отдельно сохраняет его logs даже при
падении последующих legacy checks. Успех текущего remote CI не предполагается
из локальной проверки. Аппаратные SteamOS/Gamescope/DRM, HDR и budget остаются
следующими инженерными этапами [принятого решения](ARCHITECTURE_DECISION_1_0.md).

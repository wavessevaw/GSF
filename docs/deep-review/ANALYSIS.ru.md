# Kedavra: проверка интеграции, стоимости и качества для GFG

Дата: 2026-10-22. Это инженерная ветка для основного агента, не релиз и не аппаратная приёмка.

## Версии и воспроизводимость

Проверен фактический engine GFG 3.0.0: `c264c2877a15d9b3739a346432a4fae9b8ef479d`. На момент fetch `origin/main` Kedavra указывал на `42924cc684f72fcca90a7cedf4bca101cdaa9039`; эта ветка продолжает c264c28, поэтому содержит также изменения между main и pin GFG. Не переносить её целиком на старый main без просмотра этого диапазона. HDR/post выводы относятся к c264c28.

GFG: [аудит 3.0.0](https://github.com/wavessevaw/GFG-Extreme/tree/codex/audit-v3.0.0/docs/audit-v3.0.0), [исправления контроллеров и Governor 2.0](https://github.com/wavessevaw/GFG-Extreme/tree/codex/controller-review-examples/docs/governor2). Коммит реализации GFG `b82bdd1`: 106 backend tests и 6 тестов протокола транзакций; аппаратной проверки этих изменений нет.

Локально доступен llvmpipe/Mesa, нет `/dev/dri` и подключённого Deck. Полная сборка Gamescope в этой работе не проверена. Ни один результат ниже не доказывает 30→90 отображаемых FPS, энергосбережение или сокращение задержки на Deck.

## Реальные изменения этой ветки

1. `kedavra/tools/kfg_pacing.py`: учёт отсутствующих временных слотов; отсутствие автоматического удаления длинных необъяснённых пауз; сегментация по явным исключениям/состояниям/refresh/epoch; эквивалентные дроби фаз считаются одной фазой; исходный знаменатель сохранён для проверки x4; невалидные timestamps отвергаются.
2. `tools/test_pacing.py`: семь регрессий, включая 100 кадров на 199 слотах, длинную паузу, исключение, дроби и x4. В первоначальном прогоне пяти проверкам требовалось исправление. Полные red/green логи приложены.
3. `sched/test_completion_bridge.c`: исполняемый контрпример с настоящим scheduler, а не его Python-копией. Тест добавлен в `test-sched`; тесты pacing добавлены в `make test` и CI.
4. `KFG_EXPERIMENT_NO_BLUR` — только compile-time эксперимент. По умолчанию shader не изменился: исходный и заново собранный SPIR-V SHA256 `f06dfe146d9e4113b8fd054bbe59334c77d85c76e5d1eca4e1867209fa825264`.

## P0: completion — это наблюдение, а не прогноз

`kedavra/gamescope/src/kfg/kfg.cpp`, вызов `kfg_sched_on_me(id, nVBlank + 2, true, false)` объявляет готовность пары по фиксированному прогнозу. Сам scheduler умеет учитывать готовность, но мост не сообщает наблюдаемое завершение GPU.

Контрпример: источник каждые три vblank, реальное в модели завершение через пять, 360 vblank. С текущим контрактом scheduler выбирает 237 generated кадров до модельной готовности, не переходит в degraded. При передаче callback только после завершения — 24 generated, 0 преждевременных, 2 перехода degraded. Второй вариант не достигает x3: он честно отражает нехватку бюджета.

Это **не доказательство гонки памяти**: одна Vulkan queue и barriers могут обеспечить правильный порядок. Тогда проблема проявится как ожидание поздней работы и потеря deadline, а не чтение недописанной памяти.

Контракт для реализации адаптера (ниже псевдокод, не существующие методы Gamescope):

```cpp
struct Pending {
    uint64_t epoch, pair, completion_value;
    // shared ownership of source/flow/output resources until retirement
};
void poll_completed() { // once per compositor iteration, never busy-wait
    uint64_t done = completion_counter(); // timeline or nonblocking fence status
    for (auto &p : pending) {
        if (done < p.completion_value) continue;
        if (p.epoch == active_epoch)
            scheduler_analysis_completed(p.pair, current_vblank);
        retire_resources(p); // only after actual completion, even on obsolete epoch
    }
}
```

До submit: проверить оставшийся budget с резервом под composite/scanout, а не только целевой множитель. После reset/focus/HDR/resize: отменить ещё не отправленные части старого epoch, запретить выбор их результатов, удержать уже отправленные ресурсы до completion. Не вставлять `vkWaitForFences` на каждый кадр в compositor thread. Сначала тест адаптера с delayed/out-of-order acknowledgments, затем сборка Gamescope, затем RADV + реальные flip events.

## P0: «repeat rows» не равны пропущенным слотам

Старый анализатор делил число строк repeat на число строк. Если строки отсутствуют целиком, показатель мог быть почти нулевым. Теперь внутри фиксированного refresh-сегмента:

```
unobserved = Σ max(0, floor((flip[i]-flip[i-1])/refresh + 0.5)-1)
missed_estimate = (repeat_rows + unobserved)/(rows + unobserved)
```

Сохранён `repeat_row_fraction`; новый показатель явно помечен как оценка при lossless trace. Историческая лучшая auto-трасса: 0.269% repeat → 0.775% estimated missed slots. Все пять старых трасс и раньше не проходили совокупный gate; это не превращение прежней аппаратной приёмки в провал. `historical-reanalysis.json` содержит числа и оговорки.

Ограничения: пропуск записи и пропуск flip пока неразличимы; VRR не покрыт этой моделью; интервалы вообще без generated-фаз не представлены в гистограмме source pairs. Поэтому новый анализатор тоже не является достаточным доказательством x3.

Следующий контракт телеметрии: `(epoch, present_cookie, drm_sequence, source_A, source_B, phase_num, phase_den, actual_output_kind, analysis_done, warp_done)`. `OnPresented`/FIFO intent надо сопоставлять с конкретным DRM completion cookie; `OnPageFlip` не должен терять sequence. При scene cut endpoint нельзя называть generated только потому, что scheduler его запросил. Ошибки парсинга и потери записей должны иметь отдельный счётчик и делать приёмку неопределённой.

## Стоимость: считать полный путь и хвосты

`RecordGenerate` может выполнить оставшийся analysis при приходе следующего источника. `SubmitAnalysis` делит работу после `KFG_LEVELS+3` проходов; часть finest ME/stats остаётся на следующую стадию. GPU queries composite не охватывают всю отложенную стоимость. Маленький `warp_ms` не означает дешёвый FG.

Нужны отдельные completed GPU timestamps: copy/reduce, coarse/global, finest/repair, stats, warp, post, composite, и общий интервал submit→complete. Читать queries спустя несколько кадров, без `WAIT_BIT`. Для governor нужны p95/p99 и возраст измерения, а не только среднее. Учитывать совместную нагрузку игры: длительность shader на пустом GPU не равна deadline в игре.

Старое Deck-измерение finest ME ~2.358 ms из ~4.773 ms analysis делает его разумной целью оптимизации. Но temporal/camera candidates, dedup и ранние выходы уже существуют; предлагать их «добавить» повторно неправильно. Безопасный следующий опыт: измерить долю блоков, у которых уже имеющийся confidence позволяет пропустить дополнительный repair, с отдельными gates на occlusion/slow stairs/climb. Нельзя просто повысить порог и выдать экономию за доказанную.

Шесть source textures + два output @1280×800: около 31.25 MiB SDR / 62.5 MiB HDR, без flow/post. Сокращать lifetime/ring только после доказательства максимального числа in-flight readers; иначе экономия памяти создаст overwrite hazard. CPU readback не должен появляться в горячем пути.

## Проверенное предложение по качеству: отрицательный результат тоже важен

A/B: тот же `kfg_vktest`, разрешение 1280×800, llvmpipe, все shader кроме warp идентичны. Отключение blur убирает четыре дополнительных выборки в соответствующей ветви, но это не измерение экономии ms на Deck.

На 33 фазах изменение PSNR от −0.400 dB (occlusion) до +2.268 dB (fast_pan). Dolly также ухудшается. Три фазы climb не проходят quality checks **в обеих версиях**, но намеренно исключены из общего failure counter в `kfg_vktest.c`. Общий `PASS: 0 failure(s)` нельзя описывать как прохождение всех сцен. Полные JSON и лог сохранены; `compare_engine.py` сохраняет и отрицательные результаты.

**Решение:** не отключать blur по умолчанию. Следующее изменение должно выбирать режим по уже имеющимся confidence/occlusion данным без дополнительного full-frame pass, но это гипотеза до A/B на temporal residual, shimmer и disocclusion, а не только PSNR. Простой глобальный no-blur совет отозван.

Дополнительная проверяемая гипотеза: confidence после repair должен сравнивать однородные стоимости. В selection best cost содержит prior/consistency, стоимость нового соседа — raw matching; subpixel move не пересчитывает matching cost. Не менять формулу вслепую: вынести `data_cost` и `selection_score`, записывать confidence из data_cost, проверять сцены с неоднозначной текстурой/перекрытием. Это может уменьшить артефакты без нового прохода, но дополнительные выборки могут стоить времени; «бесплатного улучшения» пока не доказано.

HDR: отдельная проверка ярких PQ границ и linear-light oracle; обычная интерполяция кодовых PQ значений не равна линейному свету. Не менять sRGB-view флаги по названию: текущая UNORM-view семантика требует проверки. Поддержка формата должна участвовать в effective FG eligibility; requested mode/cap не должны удерживать forced composition при фактическом bypass.

## Governor 2.0: единый договор с engine

Основная архитектура уже зафиксирована в linked GFG `CONCEPT.ru.md`: один supervisor, владельцы ресурсов, leases и verify/restore, пользовательское владение ресурсами, затем safety/restore → performance guard → цель режима → efficiency. Governor не должен получать право менять shader flags на каждом tick.

Engine публикует immutable snapshot с epoch и свежестью: настоящий source FPS, подтверждённая display cadence, pending GPU work, completed stage costs, effective multiplier, bypass reason, format/refresh и thermal validity. Значение unknown не заменять нулём или здоровым статусом.

Supervisor выбирает согласованный пакет: source cap, x2/x3, разрешённые post effects, TDP/CPU/GPU bounds. Применение требует ACK того же token/epoch; частичное применение → restore, не новый независимый probe. Probe CPU/GPU должен иметь исключительное владение и прерываться thermal guard. Не смешивать изменение cap, TDP и качества в одном эксперименте: иначе причинный эффект неидентифицируем.

Latency: измерять source acquire→actual flip и возраст показанного контента, отдельно input-to-photon если есть измерительное оборудование. Для interpolation x3 будущее B создаёт фундаментальное ожидание. Снижать его можно поздним выбором допустимой фазы и ограничением очереди, но не заменой метрики на CPU submit time. Пропуск просроченной генерации предпочтительнее накопления очереди; он должен отражаться в effective multiplier.

## Последовательность передачи основному агенту

1. Перенести analyzer/tests и completion counterexample; CI обязан выполнять их. Не менять quality gates ради зелёного статуса.
2. Реализовать completion adapter + epoch/resource retirement и проверить Gamescope compile; тесты reset во время split analysis, resize/HDR, delayed completion.
3. Связать DRM cookie/sequence с output metadata, добавить telemetry-loss/unknown. Только после этого проверять 30 real → 90 actual display.
4. Подключить snapshot к Governor 2.0, затем persistent transaction journal и verify/restore при unload/restart. Reference из GFG — не готовый аппаратный контроллер.
5. Отдельным измеренным патчем оптимизировать finest ME/repair, сохраняя временные quality gates. No-blur оставить экспериментом.

## Как повторить

```sh
make -C kedavra test-pacing test-sched
make -C kedavra all test-engine
# сохранить baseline build/engine.json до второго запуска
mkdir -p /tmp/kfg-no-blur
cp kedavra/build/*.spv /tmp/kfg-no-blur/
glslangValidator -V --target-env vulkan1.1 -DKFG_EXPERIMENT_NO_BLUR=1 \
  -Ikedavra/shaders -o /tmp/kfg-no-blur/kfg_warp.spv kedavra/shaders/kfg_warp.comp
kedavra/build/kfg_vktest --spv /tmp/kfg-no-blur --json /tmp/kfg-no-blur/engine.json
python3 docs/deep-review/compare_engine.py kedavra/build/engine.json /tmp/kfg-no-blur/engine.json
```

В текущей песочнице LeakSanitizer не работает под ptrace; scheduler запущен с `ASAN_OPTIONS=detect_leaks=0`, AddressSanitizer/UBSan оставлены. Это ограничение записано, а не исправлено глобальным выключением sanitizers. `evidence/` содержит результаты, включая неуспешные quality phases. Новые аппаратные испытания не проводились; ускорение и снижение энергопотребления не заявлены.

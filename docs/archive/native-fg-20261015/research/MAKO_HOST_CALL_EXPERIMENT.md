# Исполнение открытых host-цепочек без DLL

SIMULATED: скомпилированы **неизменённые** C++ constructor/render методы десяти shaderchains MAKO, `ManagedShaderBuilder`, selectors scratch и utils. Вместо Vulkan wrappers подставлен recorder. Он записывает создание изображений, форматы, размеры, descriptor image IDs, image barriers и dispatch extents. Shader registry содержит только текстовые названия: закрытые шейдеры/модели не загружались и изображение не генерировалось.

Обвязка контекста повторяет исследованный SDR path и является моделью, а не исходным `ContextImpl`. В частности, первый Gamma1 получает beta pyramid level5, дальнейшие — level6−j. Работа memory pool и GPU не моделируется. Значения ниже не обозначаются TRACE VERIFIED или DEVICE VERIFIED.

Исследованы закреплённый MAKO `edd2946bd3c4281e357bd436e0e7b80df0759576` и upstream `e933b42fcdf2768c34aa5e921aa6b76da03304c0`. Для каждого: flow1/0.75/0.5 × quality/performance ×1/2/4 outputs, семь temporal steps. Всего36 конфигураций. [Все результаты, source/harness hashes](results/mako_host_trace.json) и сжатые raw events сохранены; логические summaries обеих версий совпали по проверенным полям во всех18 сочетаниях.

## Результат при1280×800, flow1

Все числа в таблице — SIMULATED, первые две колонки не включают HDR conversion или presentation.

| Режим | Synthetic N | Dispatch | Созданных images, включая exchange | Логический payload B | Image barriers: prepass / outputs |
| --- | --- | --- | --- | --- | --- |
| Quality | 1 | 100 | 201 | 30 272 644 | 189 / 510 |
| Quality | 2 | 166 | 264 | 38 785 064 | 189 / 1020 |
| Quality | 4 | 298 | 390 | 55 809 904 | 189 / 2040 |
| Performance | 1 | 100 | 129 | 24 299 044 | 113 / 314 |
| Performance | 2 | 166 | 178 | 32 128 704 | 113 / 628 |
| Performance | 4 | 298 | 276 | 47 788 024 | 113 / 1256 |

SIMULATED: quality/performance сохраняют число dispatch и workgroups:17764 prepass и24095 на выход при flow1. Но различаются числом image channels/descriptor bindings, payload и barriers. Поэтому равное число проходов не означает равную GPU-работу. Workgroups не равны числу shader invocations: actual LocalSize закрытых модулей здесь не исследован.

SIMULATED: при переходе flow1→0.5 число prepass workgroups17764→4568, на выход24095→9300. Финальный Generate остаётся4000 workgroups (ceil1280/16 × ceil800/16), поэтому общая выходная цепочка не уменьшается строго вчетверо. GPU timing/качество при этом NOT TESTED.

SIMULATED: для quality flow1 логический prepass payload13 568 220 B; у каждого synthetic pass собственные Gamma0/Gamma1/Delta1 outputs и external destination, добавляющие8 512 420 B. Не весь scratch становится общим между разными t: Alpha0 reused scratch общ, но output-dependent buffers остаются. Тест не моделирует aliasing driver memory или реальные размеры Vulkan allocations.

SIMULATED: image binding signatures полностью совпадают для temporal steps0 и6, но не0/2 или0/3. Это проверяет эффект источников modulo2 и истории modulo3/2, а не только арифметику `lcm`. Неправильный двухфазный command cache нарушил бы привязки истории даже при чередовании правильных source images.

SIMULATED: во всех36 конфигурациях recorder не обнаружил одновременного sampled/storage binding одного image ID в одном dispatch. Это не доказательство корректной синхронизации shader accesses: не моделировались samplers, actual texel accesses, semaphore dependencies, ownership или параллельные queues.

## Воспроизведение

Нужны g++ с C++20 и Vulkan-Headers; Vulkan device, loader и LSFG не нужны:

```bash
python3 tools/trace_mako_host.py \
  --mako /path/to/pinned/MAKO \
  --headers /path/to/Vulkan-Headers/include \
  --out /tmp/new-mako-host-trace
```

Стенд компилирует host source выбранного checkout и сохраняет его SHA-256. Он проверяет34/N×66 dispatch, повтор bindings через6, корректность image IDs и ненулевые extents. Перед переносом на другую версию необходимо сверить context wiring и defaults `Image`, а не принимать успешную компиляцию за совместимость модели.

Дальнейшее решение: сравнивать GPU timestamps вместе с привязками и размерами. Объединение нескольких dispatch без учёта live images/FP16/register pressure может оказаться проигрышным. Точный occupancy и память RDNA2 остаются NOT TESTED.

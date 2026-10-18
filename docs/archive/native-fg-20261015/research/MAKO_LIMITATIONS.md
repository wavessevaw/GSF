# MAKO: ограничения исследования

- SOURCE VERIFIED: открытый backend исполняет закрытые ресурсы LSFG; открытый host graph не превращает их математику в независимую реализацию.
- SOURCE VERIFIED: актуальный source, закреплённый source GFG и опубликованный render tag отличаются. Каждый результат должен содержать source SHA, patch SHA и binary SHA.
- SOURCE VERIFIED: генерация нескольких изображений включает не только один финальный shader на каждый выход. В то же время общий prepass уже переиспользуется.
- NOT TESTED: оригинальный Windows LSFG compute graph, совпадение оригинала и Vulkan backend, качество и скорость текущего upstream на устройстве.
- NOT TESTED: полная стоимость обмена и present в gamescope. Headless quality CLI не упражняет тот же путь, что игра.
- NOT TESTED: hardware FP16 benefit. Наличие Float16 capability и успешная компиляция не определяют occupancy, частоты или энергию.
- SIMULATED: предыдущие GFG lavapipe результаты годятся для функциональной/относительной проверки того программного стенда; перенос коэффициентов ускорения на RDNA 2 не обоснован.

Gate 2 остаётся OPEN до stage measurements на Deck и привязки к реально активным source/format/present branches. Gate 1 отдельно требует исследования оригинальной LSFG версии; успешный MAKO oracle этого не заменяет.

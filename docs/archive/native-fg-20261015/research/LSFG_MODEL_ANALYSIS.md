# LSFG модели и режимы

| Вопрос | Статус и результат |
| --- | --- |
| Quality / performance | SOURCE VERIFIED: выбираются разные registry и число scratch channels (`m=2` / `m=1`) в цепочках. Изменение качества/стоимости на Deck не измерено. |
| FP16 | SOURCE VERIFIED: наличие в native resource table и поддержка Vulkan определяют возможность пути; актуальный upstream также содержит отдельную experimental conversion. Конкретная DLL ещё не проверена. |
| Flow Scale | SOURCE VERIFIED: renderer передаёт `1 / effectiveFlowScale`, backend делит исходное разрешение на этот параметр. Итоговое flow extent примерно source×UI scale, а не source/UI scale. |
| Multiplier | SOURCE VERIFIED: N выходов используют общий prepass; остальные цепочки повторяются N раз. |
| Разные версии LSFG | NOT TESTED: ни одинаковое имя DLL, ни совместимый registry не доказывают идентичность модели/математики. |

Ссылки: [создание ресурсов и инверсия scale](https://github.com/eugeniosegala/MAKO/blob/e933b42fcdf2768c34aa5e921aa6b76da03304c0/engine/mako-render/src/swapchain/resources.cpp#L198), [engine/mako-backend/src/mako.cpp:586](https://github.com/eugeniosegala/MAKO/blob/edd2946bd3c4281e357bd436e0e7b80df0759576/engine/mako-backend/src/mako.cpp#L586), [upstream shader selection](https://github.com/eugeniosegala/MAKO/blob/e933b42fcdf2768c34aa5e921aa6b76da03304c0/engine/mako-backend/src/extraction/lsfg_shader_set.cpp#L35).

HYPOTHESIS: уменьшение flow extent экономит analysis и часть промежуточных image operations; финальный Generate остаётся в source resolution. Квадратичное изменение числа flow pixels не равно такому же изменению полной GPU-стоимости.

План сравнения на одной DLL: FP32 quality flow=1 как фиксированный reference, затем отдельно FP16, performance и flow=0.75/0.5. Каждый раз измерять prepass и per-output, проверять HUD/тонкую геометрию и сохранять actual selection. Не выбирать «лучший» профиль после просмотра candidate без отдельной контрольной выборки.

# MAKO: workload и cadence — разные механизмы

SOURCE VERIFIED: `GeneratedFramePlan` хранит до четырёх нормализованных t. Полный admission сохраняет timestamp plan; частичный admission перераспределяет принятые выходы равномерно. [Владелец плана](https://github.com/eugeniosegala/MAKO/blob/e933b42fcdf2768c34aa5e921aa6b76da03304c0/engine/mako-render/src/generated_frame_plan.hpp#L15).

SOURCE VERIFIED: Fixed и Adaptive выбирают число генерируемых выходов до backend scheduling. Adaptive использует оценку настоящего интервала и целевой частоты; уменьшение реально scheduled N уменьшает выходные цепочки, но история/prepass могут сохраняться. [engine/mako-render/src/adaptive_scheduler.cpp:236](https://github.com/eugeniosegala/MAKO/blob/e933b42fcdf2768c34aa5e921aa6b76da03304c0/engine/mako-render/src/adaptive_scheduler.cpp#L236), [engine/mako-render/src/swapchain/present/admission.cpp:104](https://github.com/eugeniosegala/MAKO/blob/e933b42fcdf2768c34aa5e921aa6b76da03304c0/engine/mako-render/src/swapchain/present/admission.cpp#L104).

SOURCE VERIFIED: Smooth Cadence/presentation deadlines/acquire recovery взаимодействуют с FIFO и допуском output. Pacing уже готового изображения сам по себе не уменьшает вычисления, которые уже отправлены GPU. Пропуск ещё не отправленной generation работы — иной механизм.

HYPOTHESIS: Steady Base Cap и приоритет real frames могут улучшить конечный cadence и headroom при ограниченном TDP, но способны изменить реальную частоту и input latency. Их эффективность должна оцениваться по игровому A/B/A, а не по software Output FPS.

NOT TESTED: эффект каждой опции на LCD/OLED/VRR, стабильность настоящих кадров, optical cadence и input-to-display latency. Для каждого режима нужны requested/admitted/submitted/completed/present-handoff/display-observed counters, а также состав активной очереди. Нет оснований выбирать агрессивный multiplier только по цели 90.

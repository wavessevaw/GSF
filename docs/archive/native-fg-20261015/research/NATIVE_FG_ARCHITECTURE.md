# GFG Native FG — кандидатный контракт, НЕ утверждённая архитектура

HYPOTHESIS: transport/presentation contract можно сохранить, отделив backend selection от DLL-dependent InstanceImpl. Независимый backend должен уметь создать device/context без поиска DLL; LSFG adapter создаётся только при явно выбранном совместимом backend. Native failure ведёт к выводу настоящего кадра, а не к обязательной загрузке LSFG.

HYPOTHESIS: API пары должен принимать source identities, geometry/format, source-ready timeline, strictly ordered t[], output handles и deadline. Результат различает rejected/submitted/GPU-completed/handed-to-present; display-observed — только при соответствующем наблюдении. Один владелец выбирает генератор и отвечает за ресурсы пары. Отдельные backend не могут одновременно писать те же outputs.

HYPOTHESIS: analysis representation неизменяема для всех t одной пары. Её состав (flow/confidence/occlusion/global/layers) пока не выбран. Ограниченные пулы, retirement и source-image lifetime задаются до шейдеров. Глубина очереди ограничивается budget, teardown сохраняет in-flight ресурсы до корректного retirement.

NOT TESTED: GPU-only motion fitting, neural-free alternative, точность форматов, hardware occupancy и качество относительно LSFG. Перенос LMC fitting на GPU без доказательств не является принятой архитектурой.

Условия утверждения Gate 4: закрыты существенные вопросы Gate1/2, определены measured expensive stages Gate3, выбрана опровержимая cost/quality гипотеза, согласованы independent-startup/native-fallback tests и критерии защиты real FPS. Затем допускается один ограниченный прототип с контролируемым сравнением; не новый формальный prerelease.

Acceptance Native FG: чистая SteamOS без Lossless Scaling, DLL path отсутствует; штатный старт/вывод, 2×/3× в поддержанных режимах; отсутствие DLL opens подтверждено file trace, отсутствие закрытых payloads — manifest audit; injected backend failure сохраняет вывод real frames. В настоящее время всё перечисленное для нового продукта NOT TESTED.

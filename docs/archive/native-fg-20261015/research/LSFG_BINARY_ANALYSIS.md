# LSFG: бинарный анализ — OPEN

NOT TESTED: в облачную среду не передан оригинальный `Lossless.dll`, его версия/SHA неизвестны. PE exports/imports, D3D11/D3D12 API, DLL code paths и ресурсы конкретной установки не анализировались. Утверждать BINARY VERIFIED по исходникам MAKO нельзя.

SOURCE VERIFIED: MAKO читает PE/RT_RCDATA через `engine/mako-backend/src/extraction/dll_reader.cpp`, проверяет границы, сохраняет file SHA/resource-layout SHA и структурно валидирует compute-модули. [Контракт загрузки](https://github.com/eugeniosegala/MAKO/blob/edd2946bd3c4281e357bd436e0e7b80df0759576/engine/mako-backend/src/mako.cpp#L290). Это описание читателя MAKO, не результат вскрытия экземпляра LSFG.

SOURCE VERIFIED: актуальный MAKO сначала пробует поддержанные native SPIR-V ресурсы, затем может использовать DXBC translator. FP16 conversion является отдельным experimental path. [engine/mako-backend/src/extraction/lsfg_shader_set.cpp:35](https://github.com/eugeniosegala/MAKO/blob/e933b42fcdf2768c34aa5e921aa6b76da03304c0/engine/mako-backend/src/extraction/lsfg_shader_set.cpp#L35).

## Следующий воспроизводимый эксперимент

1. На Deck зафиксировать SHA/размер законной локальной DLL и SHA используемого MAKO CLI; запустить существующий `inspect-dll --lsfg --no-fp16` через probe.
2. Отдельно проверить FP16 и сохранить фактический отказ/выбранный путь, не только запрошенный флаг.
3. На поддерживаемой Windows-машине зафиксировать версию приложения, SHA модулей, PE imports/exports и API trace с согласованными marker frames.
4. Сопоставить идентичность используемых ресурсов и наблюдаемые входы/выходы. Разница output требует отдельного профиля oracle; MAKO не объявляется точной заменой оригинала автоматически.

Ни DLL, ни извлечённые шейдеры/веса не входят в ZIP отчёта. Бинарный анализ, восстановление вычислительного графа оригинала и применимость PIX/RenderDoc к конкретной версии остаются открытыми.

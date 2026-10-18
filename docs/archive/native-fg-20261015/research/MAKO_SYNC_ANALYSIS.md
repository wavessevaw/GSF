# MAKO: синхронизация и lifetime

SOURCE VERIFIED: backend ждёт общий source timeline, сигналит private prepass timeline, затем отдельные output timeline values. Завершение последнего выхода связано с context fence. [Prepass](https://github.com/eugeniosegala/MAKO/blob/edd2946bd3c4281e357bd436e0e7b80df0759576/engine/mako-backend/src/mako.cpp#L869), [generation](https://github.com/eugeniosegala/MAKO/blob/edd2946bd3c4281e357bd436e0e7b80df0759576/engine/mako-backend/src/mako.cpp#L907).

SOURCE VERIFIED: `prepareWork` ждёт предыдущую работу с конечным бюджетом 250 ms; command buffers и scratch переиспользуются после fence. Текущий normal source path не содержит LMC-подобного CPU motion fit между prepass и synthesis.

SOURCE VERIFIED: `ManagedShaderBuilder` вставляет image memory barriers между чтением и записью с GENERAL→GENERAL и queue-family IGNORED для внутренних image dependencies. Это не описание всех ownership transfers внешних изображений. [engine/mako-backend/src/helpers/managed_shader.cpp:95](https://github.com/eugeniosegala/MAKO/blob/edd2946bd3c4281e357bd436e0e7b80df0759576/engine/mako-backend/src/helpers/managed_shader.cpp#L95).

SOURCE VERIFIED: context retirement может быть отложен при незавершённом fence вместо немедленного уничтожения ресурсов; admission recovery проверяет backend и application render fence, затем прогревает историю. Бounded poll не означает, что любой teardown будет безопасно завершён за тот же бюджет.

NOT TESTED: корректность полного cross-device FD-path на Deck, ошибки driver/device lost, suspend/resume, swapchain recreation в реальной игре и длительное удержание source timeline. Обязательные эксперименты: пропуск source signal; отказ submit; потеря/смена swapchain; suspend/resume; проверка recovery без незавершённых ресурсов и без обязательного перехода на LSFG в независимом backend.

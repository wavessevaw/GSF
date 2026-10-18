# MAKO: архитектура из исходников

SOURCE VERIFIED: Vulkan layer и backend библиотека входят в разные компоненты, но наблюдаемый путь создаёт backend instance из процесса layer. External memory FD здесь используется для импорта изображений в private Vulkan device; сам факт FD не означает отдельный backend-процесс/IPC. [Создание ресурсов](https://github.com/eugeniosegala/MAKO/blob/e933b42fcdf2768c34aa5e921aa6b76da03304c0/engine/mako-render/src/swapchain/resources.cpp#L166).

SOURCE VERIFIED: `entrypoint.cpp` перехватывает `vkQueuePresentKHR`; swapchain present modules разделяют admission, копирование реального кадра, выдачу синтетических и retirement. Backend строит закрытые compute pipelines из локальной DLL. Common владеет Vulkan wrappers и учётом памяти. CLI даёт отдельный исследовательский путь без game presentation.

SOURCE VERIFIED: два exported source images, пул destination images и общий exported timeline создаются в `buildFrameGenerationResources`; backend импортирует FD. Эти объекты живут дольше одного кадра. MAKO не обязан загружать/компилировать модель на каждый синтетический выход.

SOURCE VERIFIED: GFG te.8 добавляет adapter и source-format/Fusion изменения поверх `edd2946bd3c4281e357bd436e0e7b80df0759576`. Upstream `e933b42fcdf2768c34aa5e921aa6b76da03304c0` — другой объект исследования, не включённый автоматически в te.8. У него добавлены DXBC translation и frame profiling, поэтому versioned comparison обязателен.

NOT TESTED: выигрыш на gamescope, стоимость межустройственного обмена и точный состав активных ветвей на Deck. Реальную конфигурацию подтвердят local inventory, runtime logs и trace, не схема.

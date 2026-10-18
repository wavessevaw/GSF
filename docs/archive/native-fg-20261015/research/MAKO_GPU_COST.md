# MAKO: что измерять и что уже поддерживает upstream

SOURCE VERIFIED: актуальный upstream содержит `Instance::enableFrameProfiling/readFrameProfile`. `readFrameProfile` возвращает GPU prepass/motion estimation, per-output synthesis/conversion, общий GPU span и свойства timestamp counter. [Реализация](https://github.com/eugeniosegala/MAKO/blob/e933b42fcdf2768c34aa5e921aa6b76da03304c0/engine/mako-backend/src/mako.cpp#L868). CLI `benchmark --profile` имеет warmup/samples и предупреждает о возмущении таймингов query reads; результаты не являются capacity FPS.

SOURCE VERIFIED: в основе GFG `edd2946bd3c4281e357bd436e0e7b80df0759576` этого интерфейса нет. Перед запуском фиксировать SHA CLI и help, не передавать новые флаги старой сборке. Доступный probe пока использует общий для обеих версий quality interface.

NOT TESTED: GPU transport/sync/presentation cost на Deck. Внутренний profile backend не охватывает application-device source/output copy и Gamescope. Цифры из него нельзя выдавать за полную стоимость FG.

План: на одном Deck закрепить runtime/модель/precision/flow, выполнить N=1/2/4 с одинаковыми inputs; затем full-game A/B/A. Раздельно source-copy timestamps, model timestamps, output-copy timestamps и host wait intervals. Fresh context и холодный startup не смешивать со steady-state. Сохранять raw samples, p50/p95/p99, частоты и температуру.

HYPOTHESIS: импорт FD в setup может быть несущественен на пару, а copy/layout/sync в каждом present — существенны. Ни «zero-copy», ни дорогой IPC не следуют из наличия external-memory API; нужно проверить активные копирования и реальные GPU spans.

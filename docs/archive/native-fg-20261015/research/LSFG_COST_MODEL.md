# Модель стоимости LSFG — неизвестные не заменены числами

SOURCE VERIFIED: host-side модель MAKO имеет общий prepass A и выходные цепочки S(t). Для N синтетических изображений работа модели имеет вид `A + Σ S(t_i)`. Свойства оригинального Windows runtime за пределами этой модели не подтверждены.

HYPOTHESIS / план идентификации: на одном устройстве/входах/модели измерить N=1,2,4, отдельно A и каждый S; проверить, насколько пригодна регрессия `T_model(N)=a+bN`. Нелинейность возможна из-за cache, размещения scratch, clock/power, очереди, синхронизации и разных timestamps. Интерцепт по двум full-pipeline замерам не доказывает цену анализа.

Нужны четыре независимых измерительных домена:

| Домен | Измерение | Пока |
| --- | --- | --- |
| Model work | GPU stage timestamps A, S_i, conversion | NOT TESTED на Deck |
| Transport | Source copy/export-import setup/output copy; steady-state отдельно от setup | NOT TESTED |
| Synchronization | CPU wait, queue gap, admission/acquire wait; не складывать с перекрывающимся GPU work | NOT TESTED |
| Presentation | Requested/submitted/completed/handed to present/displayed отдельно | NOT TESTED |

Полный GPU-work budget можно складывать для непересекающихся участков исполнения одного домена. Critical path до ready/presented считать по связям событий. Сумма wall waits и GPU durations часто считает одно и то же дважды. Между разными устройствами/очередями нельзя вычитать несопоставимые timestamps без калибровки.

`≤2 ms/synthetic frame` проверять с отдельным A, а также с амортизированным `(A+ΣS)/N`; не делить стоимость пары на число всех выходов, включающее настоящие кадры. Цель −20% относительно MAKO+LSFG остаётся NOT TESTED. Реальный FPS игры, cadence и энергия имеют приоритет над одиночным временем шейдера.

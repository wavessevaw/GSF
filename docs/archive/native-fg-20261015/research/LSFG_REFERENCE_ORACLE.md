# Reference oracle — протокол и исполняемый локальный адаптер

SOURCE VERIFIED: существующий MAKO CLI уже умеет `inspect-dll`, `quality-regression --sequence-plan`, сохраняет previous/current/reference/generated PPM, источники и `sequence.tsv` с нормализованными t и параметрами сцены. Повторно реализовывать этот генератор тестовых сцен не требуется.

Готов адаптер [native_fg_probe.py](../tools/native_fg_probe.py), использующий установленный MAKO CLI. Без `--oracle` он не запускает LSFG. При явном `--oracle` сначала проверяет DLL, затем запускает 1280×800 sequence: десять history steps и две пары с выбранными t. Для 3× это t=1/3,2/3; поддержаны также 2× и 5×. Полный архив содержит изображения, конфигурацию, SHA inputs/tool, stdout/stderr, exit status и SHA артефактов. DLL/модели и извлечённые шейдеры не упаковываются. `GFG_TEMPORAL_ENGINE=off` явно исключает подмену LSFG на TE в GFG-patched CLI.

```bash
python3 native_fg_probe.py --vulkan --oracle   --mako-cli /absolute/path/to/mako-cli   --dll '/absolute/path/to/Lossless.dll'   --scene camera-pan --multiplier 3 --flow 1   --out ~/Desktop/gfg-lsfg-reference.zip
```

NOT TESTED: licensed run ещё не получен. Это начальный MAKO+LSFG reference adapter, не готовый oracle оригинального Windows Lossless Scaling и не полный benchmarking framework. CLI PASS означает только прохождение его локальных порогов; качественная приёмка GFG и hardware gates этим не закрываются.

Ограничения: процедурные сцены upstream, нормализованное время вместо реальных 30 FPS, всего две измеряемые пары, host readback каждого output. Это smoke/quality packet, непригодный для измерения throughput/мощности/задержки. Источник математики reference сцены открыт, но дальнейший fair comparison должен подавать сохранённые источники кандидатам без перерисовки другой сцены.

Следующие расширения только после успешного local run: все пять сцен, смена текста/scene cuts/прозрачность, более длинная temporal sequence; Windows capture с проверкой порядка marker frames; сравнение MAKO output с оригиналом при одинаковой модели и параметрах. Повторное вычисление метрик FLIP/SSIM/LPIPS не заменяет визуальную проверку локальных ошибок.

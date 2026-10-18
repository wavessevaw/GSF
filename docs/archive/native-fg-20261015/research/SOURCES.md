# Зафиксированные источники

| Источник | SHA | Назначение |
| --- | --- | --- |
| GFG te.8 | `a7f424af20cfb56efdbddf3c4e8e4bde414f3c0d` | Опубликованная база, исследование не меняет релиз |
| MAKO, основа GFG | `edd2946bd3c4281e357bd436e0e7b80df0759576` | Чистый git archive, без наложенного GFG patch |
| MAKO, main на момент исследования | `e933b42fcdf2768c34aa5e921aa6b76da03304c0` | Отдельный чистый clone; не подмена зависимости GFG |
| MAKO render-v4.0.0 | `dcb961b942fdbb4528b909af51d1b8a1a14d2c18` | Ссылка на опубликованный тег, отличается от основы GFG |
| lsfg-vk v1.0.0 | `7113d7d02da9fc9df5cb3b03230d1f7de86f7056` | Исторический открытый host-код |
| lsfg-vk HEAD | `0af010df9756bbd10d8d92de0fc09b301f9e1cb8` | Только README о переезде; не действующий исходный движок |

SOURCE VERIFIED: исторический `framegen/v3.1p_src/context.cpp:83–119` уже содержит общий mipmaps/alpha/beta и цикл gamma/delta/generate для каждого выхода. Это не независимое наблюдение оригинальной Windows-программы. Проприетарные ресурсы из исследовательских исходников в GFG не копировались.

| Запрошенный PR | SHA головы | Проверенный предмет |
| --- | --- | --- |
| #86 | `0a0754ec5cca9a408657c8d9b1bd0b8828ba16c4` | GFG Open, colour-flow, native timing |
| #123 | `57e292605f7b294a6327d5b67cedf7a70074bb3d` | Frame Fusion Core, общий scaler input до/после FG |
| #125 | `ff0cef04156fccb65a014a1f50992cad1a1ed716` | Native-resolution source transport |
| #129 | `94187ea99dce1d13a6567889ff6d99c327cc3c07` | Temporal Engine/LMC; последний commit головы касается Shell, поэтому одного commit subject недостаточно |

В этой среде проверены локальные исходники и ссылки PR. Два ранее переданных Deck-лога относятся к te.7 и разным играм; они не образуют A/B-сравнение te.7 и te.8. Нового аппаратного сравнения нет.

## Воспроизведение source inventory

```sh
git clone https://github.com/eugeniosegala/MAKO.git /tmp/mako-research
git -C /tmp/mako-research archive edd2946bd3c4281e357bd436e0e7b80df0759576 | tar -x -C /path/to/empty-base
# Аналогично выгрузить current SHA в другой пустой каталог.
python3 tools/audit_native_fg_sources.py --mako-base /path/to/empty-base --mako-current /path/to/current --out /tmp/inventory.json
```

[Машинный inventory](results/source_inventory.json) содержит SHA-256 изученных файлов и номера строк якорей. Dispatch model — явно заданная после чтения кода арифметика с проверками структуры, не полноценный анализатор C++ и не runtime trace. При изменении исходников модель требует повторной проверки.

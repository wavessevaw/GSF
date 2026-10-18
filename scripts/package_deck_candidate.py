#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package current actual native GPU layer; no test-only virtual WSI hooks."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    kit = args.out/'GSF-Deck-Test'
    kit.mkdir(parents=True, exist_ok=False)
    for name in ('libVkLayer_gfg_native.so','wsi-gpu-smoke'):
        shutil.copy2(args.build/name, kit/name)
    (kit/'layers').mkdir()
    manifest = json.loads((args.build/'VkLayer_gfg_native.json').read_text())
    manifest['layer']['library_path'] = '../libVkLayer_gfg_native.so'
    (kit/'layers/VkLayer_gfg_native.json').write_text(json.dumps(manifest,indent=2)+'\n')
    for name in ('run-test.py','GSF-Test.desktop','game-mode.py','install-game-mode.py','Install-Game-Mode.desktop'):
        shutil.copy2(ROOT/'engine/gfg-native/candidate'/name, kit/name)
    (kit/'GSF-Test.desktop').chmod(0o755)
    (kit/'Install-Game-Mode.desktop').chmod(0o755)
    (kit/'wsi-gpu-smoke').chmod(0o755)
    commit = subprocess.check_output(['git','rev-parse','HEAD'], cwd=ROOT, text=True).strip()
    dirty = subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=ROOT,text=True)
    if dirty: raise ValueError('refuse a dirty source build')
    (kit/'BUILD_INFO.json').write_text(json.dumps({'source_commit':commit, 'experimental':True,
        'architecture':'x86_64','virtual_wsi':False,'physical_scanout_verified':False,
        'scope':'controlled Vulkan test application, not game qualification'},indent=2)+'\n')
    (kit/'README.txt').write_text('''GSF — автономный тестовый кандидат (экспериментальный).
Распакуйте всю папку GSF-Deck-Test в доступное для записи место на Deck.
В Desktop Mode откройте GSF-Test.desktop. KDE может попросить разрешить запуск.
Ручная установка, sudo, копирование библиотек и параметры запуска Steam не нужны.
Пакет запускает собственное контрольное приложение: baseline, 2x tiles,
2x GPU LMC, пересоздание, изменение размера и интервалы вывода 5 → 10 кадров/с.
Маленькие окна проверяют вычисления, а не видимую плавность. После проверок
обычный запуск показывает A/B в окнах 480×320: A — 5 исходных кадров/с,
B — GPU LMC 5 → 10 кадров/с. Каждый фрагмент движется примерно 8 секунд.
Это замедленный диагностический пример, а не проверка игры при 90 Гц.
--noninteractive пропускает увеличенный A/B; --visual включает его явно.
На холодном кэше проверка может занимать несколько минут. Дождитесь результата.
Готовый ZIP с диагностикой находится в ~/GSF-Test-Results/.
Он содержит журналы теста и сведения о сборке; ничего автоматически не отправляется.
Этот пакет не подключает генератор к вашим играм и не доказывает OLED 90 Гц.
Game Mode: откройте Install-Game-Mode.desktop в Desktop Mode.
Установщик проверит и скопирует пакет в постоянную пользовательскую папку.
Установщик сам запросит добавление GSF Game Mode Test через штатный
steamos-add-to-steam. Steam может показать своё подтверждение.
Затем вернитесь в Game Mode, выберите 90 Гц и отключите лимит FPS
для этого теста. Совместимость Proton не включать: приложение Linux.
Запустите GSF Game Mode Test из библиотеки. Весь прогон автоматический.
Он проверяет baseline/LMC 30→60 и 45→90 на 640×400 и 45→90 на 1280×800.
Настройки экрана/лимитера сохраняются в отчёте лишь косвенно через тайминги;
тест не меняет их и не доказывает физический показ OLED.
Последний отчёт Game Mode: ~/GSF-Test-Results/GSF-Game-Mode-Latest.zip
Отдельный архив каждого прогона сохраняется рядом, в том числе при ошибках.
Запуск напрямую через Python: python3 run-test.py
''')
    for name in ('LICENSE.md','NOTICE.md','COPYRIGHT.md'):
        shutil.copy2(ROOT/name, kit/name)
    with (kit/'SOURCE.tar.gz').open('wb') as source:
        subprocess.run(['git','archive','--format=tar.gz','HEAD','engine/gfg-native','engine/gfg-temporal',
                        'scripts/package_deck_candidate.py','LICENSE.md','NOTICE.md','COPYRIGHT.md','CONTRIBUTING.md'],
                       cwd=ROOT,stdout=source,check=True)
    hashes={p.relative_to(kit).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in kit.rglob('*') if p.is_file()}
    (kit/'CHECKSUMS.json').write_text(json.dumps(hashes,indent=2)+'\n')
    archive = args.out/'GSF-Deck-Test.zip'
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
        for p in sorted(kit.rglob('*')):
            if p.is_file(): z.write(p, 'GSF-Deck-Test/'+p.relative_to(kit).as_posix())
    print(archive)

if __name__=='__main__':
    main()

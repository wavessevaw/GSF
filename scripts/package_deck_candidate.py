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
    for name in ('libVkLayer_gfg_native.so','VkLayer_gfg_native.json','wsi-gpu-smoke'):
        shutil.copy2(args.build/name, kit/name)
    for name in ('run-test.py','GSF-Test.desktop'):
        shutil.copy2(ROOT/'engine/gfg-native/candidate'/name, kit/name)
    (kit/'GSF-Test.desktop').chmod(0o755)
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
2x GPU LMC, пересоздание и изменение размера. Возможны небольшие тестовые окна.
На холодном кэше проверка может занимать несколько минут. Дождитесь результата.
Готовый ZIP с диагностикой находится в ~/GSF-Test-Results/.
Он содержит журналы теста и сведения о сборке; ничего автоматически не отправляется.
Этот пакет не подключает генератор к вашим играм и не доказывает OLED 90 Гц.
Запуск напрямую через Python: python3 run-test.py
''')
    for name in ('LICENSE.md','NOTICE.md','COPYRIGHT.md'):
        shutil.copy2(ROOT/name, kit/name)
    with (kit/'SOURCE.tar.gz').open('wb') as source:
        subprocess.run(['git','archive','--format=tar.gz','HEAD','engine/gfg-native','engine/gfg-temporal',
                        'scripts/package_deck_candidate.py','LICENSE.md','NOTICE.md','COPYRIGHT.md','CONTRIBUTING.md'],
                       cwd=ROOT,stdout=source,check=True)
    hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in kit.iterdir() if p.is_file()}
    (kit/'CHECKSUMS.json').write_text(json.dumps(hashes,indent=2)+'\n')
    archive = args.out/'GSF-Deck-Test.zip'
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
        for p in sorted(kit.iterdir()):z.write(p, 'GSF-Deck-Test/'+p.name)
    print(archive)

if __name__=='__main__':
    main()

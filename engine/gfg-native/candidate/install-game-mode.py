#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify, install a private bundle, request its native SteamOS shortcut."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

KIT=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('gsf_candidate',KIT/'run-test.py')
base=importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def write_atomic(path,text,mode=0o600):
    if path.is_symlink():raise ValueError('Не заменяется символическая ссылка: '+str(path))
    fd,name=tempfile.mkstemp(prefix='.'+path.name+'-',dir=path.parent)
    try:
        with os.fdopen(fd,'w') as f:f.write(text)
        os.chmod(name,mode)
        os.replace(name,path)
    finally:
        if os.path.exists(name):os.unlink(name)


def desktop_quote(text):
    # Desktop Exec escaping, not shell quoting. Percent is a field-code escape.
    for char in ('\\','"','`','$'):text=text.replace(char,'\\'+char)
    return '"'+text.replace('%','%%')+'"'


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--noninteractive',action='store_true')
    args=ap.parse_args()
    root=Path.home()/'.local/share/gsf-test-game-mode'
    root.mkdir(parents=True,exist_ok=True)
    result={'status':'FAIL','steam_add_status':'not-requested'}
    code=1
    try:
        base.verify_payload(KIT)
        build=json.loads((KIT/'BUILD_INFO.json').read_text())
        sha=build['source_commit']
        if not re.fullmatch('[0-9a-f]{40}',sha):raise ValueError('Неверный идентификатор сборки')
        target=root/sha
        if target.is_symlink():raise ValueError('Неверная папка установки')
        if target.exists():base.verify_payload(target)
        else:
            stage=Path(tempfile.mkdtemp(prefix='.install-',dir=root))
            try:
                for name in (*base.PAYLOAD,'CHECKSUMS.json'):
                    dst=stage/name;dst.parent.mkdir(parents=True,exist_ok=True)
                    shutil.copy2(KIT/name,dst)
                base.verify_payload(stage)
                (stage/'wsi-gpu-smoke').chmod(0o700)
                stage.rename(target)
            finally:
                if stage.exists():shutil.rmtree(stage)
        # A stable executable keeps the Steam shortcut valid across test updates.
        launch=root/'GSF-Game-Mode-Test.sh'
        write_atomic(launch,'#!/bin/sh\nexec /usr/bin/python3 '+shlex.quote(str(target/'game-mode.py'))+' "$@"\n',0o700)
        apps=Path.home()/'.local/share/applications';apps.mkdir(parents=True,exist_ok=True)
        entry=apps/'GSF-Game-Mode-Test.desktop'
        marker='X-GSF-Test-Launcher=true'
        if entry.exists() and marker not in entry.read_text():raise ValueError('Пункт меню уже занят другим приложением')
        write_atomic(entry,'[Desktop Entry]\nType=Application\nName=GSF Game Mode Test\nExec='+desktop_quote(str(launch))+
                     '\nPath='+str(root)+'\nIcon=utilities-system-monitor\nTerminal=false\nCategories=Game;\n'+marker+'\n',0o700)
        added=root/'STEAM_ADD_REQUEST.json'
        if added.exists():
            state=json.loads(added.read_text())
            if state.get('desktop_entry')!=str(entry):raise ValueError('Неверная запись установки')
            result['steam_add_status']='previously-requested-unverified'
        else:
            helper=shutil.which('steamos-add-to-steam')
            if not helper:raise ValueError('Не найден штатный steamos-add-to-steam. Пункт GSF Game Mode Test подготовлен в меню приложений.')
            p=subprocess.run([helper,str(entry)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
            (root/'steam-add.log').write_text(p.stdout)
            if p.returncode:raise ValueError('Steam не принял запрос добавления, см. steam-add.log')
            write_atomic(added,json.dumps({'desktop_entry':str(entry),'status':'requested-unverified'})+'\n')
            result['steam_add_status']='requested-unverified'
        result.update(status='PASS',bundle=str(target),desktop_entry=str(entry))
        code=0
        message='Пакет проверен и установлен. Запрос добавления GSF Game Mode Test отправлен в Steam.\nПерейдите в Game Mode и запустите его из библиотеки.\nДля 45 → 90: экран 90 Гц, лимит FPS выключен, Proton не включать.\nВесь прогон и ZIP отчёта автоматические: ~/GSF-Test-Results/.\nSteam может показать собственное окно подтверждения добавления.'
    except Exception as error:
        result['error']=str(error)
        message='Установка GSF: '+str(error)
    write_atomic(root/'INSTALL_RESULT.json',json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(message,flush=True)
    if not args.noninteractive and shutil.which('kdialog'):
        subprocess.run(['kdialog','--msgbox' if not code else '--error',message],check=False)
    return code


if __name__=='__main__':raise SystemExit(main())

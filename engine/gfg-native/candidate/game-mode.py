#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual GPU LMC qualification client launched by Steam, not game injection."""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import zipfile

KIT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('gsf_candidate', KIT/'run-test.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def inspect_case(log, telemetry, frames, source, output):
    rate = re.search(r'GSF_SOURCE_RATE .*steady_hz=([0-9.]+) absolute=1', log)
    if not rate or float(rate[1]) < source*0.9:
        raise ValueError('Источник не выдержал заданную частоту; проверьте лимитер/журнал')
    result = {'source_hz_measured':float(rate[1])}
    if not output:
        if 'GPU bridge active:' in log:
            raise ValueError('Генератор неожиданно включён в baseline')
        return result
    c = base.counters(log)
    expected = {'bridges':1, 'real':frames, 'errors':0,
                'present_path_cpu_waits':0, 'present_path_host_pixel_reads':0}
    if any(c.get(k) != v for k,v in expected.items()):
        raise ValueError('Не прошли счётчики GPU bridge: '+json.dumps(c))
    minimum = max(5, math.ceil((frames-1)*0.9))
    if c.get('synthetic',0) < minimum or c.get('synthetic',0)+c.get('skipped',0) != frames-1:
        raise ValueError('Не выдержан бюджет промежуточных кадров: '+json.dumps(c))
    if c.get('verify_checks',0) < 1 or c.get('verify_distinct') != c.get('verify_checks'):
        raise ValueError('Нет подтверждения отличающихся GPU-пикселей')
    marker = 'GSF_LOADED_LAYER path='+str(KIT/'libVkLayer_gfg_native.so')+' verified=1'
    if marker not in log:
        raise ValueError('Не подтверждена фактически загруженная библиотека')
    pairs = [json.loads(s) for s in telemetry.read_text().splitlines() if s.strip()]
    pairs = [p for p in pairs if p.get('event')=='pair']
    period = 1_000_000_000//output
    tolerance = period*0.3
    uniform, transitions, accepted, steady_accepted = [], [], [], []
    for p in pairs:
        if p['refresh_period_ns'] != period:
            raise ValueError('Неверный такт вывода')
        a, t = p['wsi_accepted_ns'], p['target_ns']
        handoffs=[a[k] for k in (0,2) if a[k]]
        accepted += handoffs
        if p['pair']>3:steady_accepted += handoffs
        if a[0] and a[2]:
            gap, planned = a[2]-a[0], t[2]-t[0]
            if planned not in (period, 2*period) or abs(gap-planned)>tolerance:
                raise ValueError(f'Нарушен интервал G1/real: {gap/1e6:.3f} мс')
            (uniform if planned==period else transitions).append(gap/1e6)
    if len(uniform) < minimum-2:
        raise ValueError('Недостаточно равномерных пар: '+str(len(uniform)))
    if len(accepted)<2 or any(b<=a for a,b in zip(accepted,accepted[1:])):
        raise ValueError('Нарушен порядок отправки кадров')
    if len(steady_accepted)<2:raise ValueError('Недостаточно данных после старта')
    measured = (len(steady_accepted)-1)*1e9/(steady_accepted[-1]-steady_accepted[0])
    if measured < output*0.9:
        raise ValueError(f'Фактическая отправка {measured:.2f} Гц ниже бюджета {output} Гц')
    result.update(counters=c, output_hz_measured=measured, output_rate_scope='after first 3 pairs',
                  actual_handoffs=len(accepted), uniform_pairs=len(uniform),
                  g1_to_real_ms_min=min(uniform), g1_to_real_ms_max=max(uniform),
                  transition_gaps_ms=transitions)
    return result


def profiles(smoke):
    targets = [(96,64,5,10,12)] if smoke else [
        (480,320,5,10,40), (640,400,30,60,180),
        (640,400,45,90,270), (1280,800,45,90,270)]
    for width,height,source,output,frames in targets:
        for active in (False,True):
            yield width,height,source,output,frames,active


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--smoke',action='store_true',help='Короткая проверка запуска, не 90 Гц')
    ap.add_argument('--results',type=Path,default=Path.home()/'GSF-Test-Results')
    args=ap.parse_args()
    args.results.mkdir(parents=True,exist_ok=True)
    run=Path(tempfile.mkdtemp(prefix=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-game-'),dir=args.results))
    keys=('XDG_CURRENT_DESKTOP','XDG_SESSION_TYPE','SteamAppId','SteamGameId',
          'GAMESCOPE_WAYLAND_DISPLAY','ENABLE_GAMESCOPE_WSI','DISPLAY','WAYLAND_DISPLAY',
          'VK_INSTANCE_LAYERS','VK_LAYER_PATH','VK_ADD_LAYER_PATH','STEAM_RUNTIME')
    session={k:os.environ[k] for k in keys if k in os.environ}
    hints=' '.join(session.get(k,'') for k in ('XDG_CURRENT_DESKTOP','GAMESCOPE_WAYLAND_DISPLAY','WAYLAND_DISPLAY'))
    report={'status':'FAIL','stage':'preflight','suite':'launch-smoke' if args.smoke else 'game-mode',
            'session':session,'gamescope_environment_hint':'gamescope' in hints.lower(),
            'physical_scanout':'UNVERIFIED','platform':platform.platform(),'cases':{}}
    code=1
    try:
        base.verify_payload(KIT)
        report['payload_verified']=True
        report['build']=json.loads((KIT/'BUILD_INFO.json').read_text())
        report['library_sha256']=base.digest(KIT/'libVkLayer_gfg_native.so')
        order=base.prepare_gamescope_layer(run)
        if order:report['gamescope_layer_order']=order['metadata']
        if not args.smoke and not (os.environ.get('SteamAppId') or os.environ.get('SteamGameId')):
            raise ValueError('Запустите GSF Game Mode Test из библиотеки Steam в Game Mode')
        if platform.machine()!='x86_64' or not os.environ.get('DISPLAY'):
            raise ValueError('Нужны Linux x86_64 и XWayland DISPLAY')
        (KIT/'wsi-gpu-smoke').chmod((KIT/'wsi-gpu-smoke').stat().st_mode|0o100)
        for name in ('libVkLayer_gfg_native.so','wsi-gpu-smoke'):
            p=subprocess.run(['ldd',str(KIT/name)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=15)
            (run/(name+'.dependencies.txt')).write_text(p.stdout)
            if p.returncode or 'not found' in p.stdout:raise ValueError('Недоступны библиотеки: '+name)
        failed=False
        for w,h,source,output,frames,active in profiles(args.smoke):
            label=f'{w}x{h}-{source}to{output}-'+('B-LMC' if active else 'A-baseline')
            report['stage']=label
            print('GSF Game Mode: '+label,flush=True)
            env=base.child_env(KIT,active,False,'native-first',preserve_layers=True)
            if order:
                env.update(order['disable_environment'])
                env['VK_LAYER_PATH']=str(order['directory'])+':'+env['VK_LAYER_PATH']
            env.update(GFG_TEST_WIDTH=str(w),GFG_TEST_HEIGHT=str(h),GFG_TEST_SOURCE_HZ=str(source),
                       GFG_TEST_FRAMES=str(frames),GFG_TEST_ABSOLUTE_PACING='1',
                       GFG_TEST_TITLE='GSF '+label,
                       GFG_TEST_BADGE=(f'B {source}>{output} FPS' if active else f'A {source} FPS'))
            if active:
                env.update(GFG_NATIVE_ALGO='lmc-gpu',GFG_NATIVE_OUTPUT_HZ=str(output),
                           GFG_TEST_ALLOW_SKIPS='1',GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY=str(max(2,source)),
                           GFG_NATIVE_TELEMETRY=str(run/(label+'.jsonl')))
            observed={'requested_layers':env.get('VK_INSTANCE_LAYERS','')}
            try:
                p=subprocess.run([str(KIT/'wsi-gpu-smoke')],env=env,stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,text=True,timeout=180)
                (run/(label+'.log')).write_text(p.stdout)
                if 'GFG native: gpu stats ' in p.stdout:
                    observed['counters']=base.counters(p.stdout)
                gate=re.search(r'GSF_REAL_WSI_GATE ([^\n]+)',p.stdout)
                if gate:observed['create_gate']=gate[1]
                if active and observed.get('counters',{}).get('bridges') == 0:
                    raise ValueError('Генератор не включился: '+observed.get('create_gate','нет create gate'))
                if p.returncode or re.search(r'VUID-|VALIDATION ERROR|Validation Error',p.stdout):
                    raise ValueError(f'Ошибка Vulkan/приложения, exit={p.returncode}')
                if 'GPU bridge WSI smoke PASS' not in p.stdout:raise ValueError('Нет завершения приложения')
                metrics=inspect_case(p.stdout,run/(label+'.jsonl'),frames,source,output if active else 0)
                report['cases'][label]={'status':'PASS',**observed,**metrics}
            except (ValueError,subprocess.TimeoutExpired) as error:
                if isinstance(error,subprocess.TimeoutExpired):
                    out=error.stdout or b''
                    (run/(label+'.log')).write_text(out.decode(errors='replace') if isinstance(out,bytes) else out)
                report['cases'][label]={'status':'FAIL',**observed,'error':str(error)}
                failed=True
        report.update(status='FAIL' if failed else 'PASS',stage='complete')
        code=int(failed)
    except (Exception,KeyboardInterrupt) as error:
        report['error']=str(error) or 'Прервано'
    finally:
        (run/'REPORT.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
        text='GSF Game Mode: '+report['status']+'\n'+report.get('error','')+'\n'
        text+='\n'.join(k+': '+v['status']+' '+v.get('error','') for k,v in report['cases'].items())
        text+='\nФизический вывод OLED: НЕ ПОДТВЕРЖДЁН.\n'
        (run/'RESULT.txt').write_text(text)
        archive=run.with_suffix('.zip')
        with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
            for p in sorted(run.rglob('*')):
                if p.is_file():z.write(p,run.name+'/'+p.relative_to(run).as_posix())
        latest=args.results/'GSF-Game-Mode-Latest.zip'
        fd,tmp=tempfile.mkstemp(prefix='.latest-',dir=args.results);os.close(fd)
        try:
            shutil.copyfile(archive,tmp);os.replace(tmp,latest)
        finally:
            if os.path.exists(tmp):os.unlink(tmp)
        print(text+'\nАрхив: '+str(latest),flush=True)
    return code


if __name__=='__main__':raise SystemExit(main())

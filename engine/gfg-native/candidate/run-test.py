#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable GSF candidate: one launch, verified payload, isolated tests and report."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

KIT = Path(__file__).resolve().parent
PAYLOAD = ('libVkLayer_gfg_native.so', 'layers/VkLayer_gfg_native.json', 'wsi-gpu-smoke',
           'run-test.py', 'GSF-Test.desktop', 'game-mode.py', 'install-game-mode.py',
           'Install-Game-Mode.desktop', 'BUILD_INFO.json', 'README.txt',
           'LICENSE.md', 'NOTICE.md', 'COPYRIGHT.md', 'SOURCE.tar.gz')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_payload(kit):
    expected = json.loads((kit/'CHECKSUMS.json').read_text())
    if set(expected) != set(PAYLOAD):
        raise ValueError('Неполный список файлов пакета')
    for name, checksum in expected.items():
        p = kit/name
        if p.is_symlink() or not p.is_file() or digest(p) != checksum:
            raise ValueError('Повреждён или отсутствует файл: '+name)
    manifest = json.loads((kit/'layers/VkLayer_gfg_native.json').read_text())['layer']
    if manifest['name'] != 'VK_LAYER_GFG_native' or manifest['library_path'] != '../libVkLayer_gfg_native.so':
        raise ValueError('Неверный manifest Vulkan-слоя')


def counters(log):
    lines = [s for s in log.splitlines() if s.startswith('GFG native: gpu stats ')]
    if len(lines) != 1:
        raise ValueError('Нет однозначного подтверждения загрузки GPU bridge')
    return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)', lines[0])}


def check_active(log, lifecycle, kit, paced=False, frames=12):
    c = counters(log)
    bridges = 3 if lifecycle else 1
    if lifecycle: frames = 36
    required = {'bridges':bridges, 'real':frames, 'synthetic':frames-bridges,
                'skipped':0, 'errors':0, 'present_path_cpu_waits':0, 'present_path_host_pixel_reads':0}
    if paced:
        # Cold initialization may invalidate history; deadline misses remain
        # visible and are not counted as generated frames. Strict cases above
        # still require every pair, with zero skips.
        required.pop('synthetic')
        required.pop('skipped')
        if c.get('synthetic', 0) < frames-4 or c.get('synthetic', 0) + c.get('skipped', 0) != frames-bridges:
            raise ValueError('Недостаточно фактически выведенных кадров в проверке такта')
    for k, value in required.items():
        if c.get(k) != value:
            raise ValueError(f'Не прошла проверка {k}: {c.get(k)} вместо {value}')
    if c.get('verify_checks', 0) < bridges or c.get('verify_distinct') != c.get('verify_checks'):
        raise ValueError('Нет подтверждения отличающихся промежуточных GPU-пикселей')
    marker = 'GSF_LOADED_LAYER path='+str(kit/'libVkLayer_gfg_native.so')+' verified=1'
    if marker not in log:
        raise ValueError('Не подтверждён фактический путь загруженной библиотеки')
    if lifecycle:
        records = re.findall(r'GSF_LIFECYCLE_RETIRED generation=(\d+) real=(\d+) synthetic=(\d+) pixel_checks=(\d+) status=(\w+)', log)
        if len(records) != 3 or any(int(g)!=i or int(r)!=12 or int(s)!=11 or int(p)<1 or status!='PASS'
                for i,(g,r,s,p,status) in enumerate(records)):
            raise ValueError('Нет полного восстановления LMC во всех трёх swapchain')
        if 'extent=128x80' not in log:
            raise ValueError('Не подтверждено изменение размера')
    return c


def check_cadence(path, minimum=5):
    pairs = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    pairs = [p for p in pairs if p.get('event') == 'pair']
    gaps, transitions = [], []
    for pair in pairs:
        if pair['refresh_period_ns'] != 100_000_000:
            raise ValueError('LMC не включил заданный выходной такт 10 Гц')
        accepted = pair['wsi_accepted_ns']
        if accepted[0] and accepted[2]:
            gap = accepted[2] - accepted[0]
            planned = pair['target_ns'][2] - pair['target_ns'][0]
            if planned not in (100_000_000, 200_000_000) or abs(gap-planned) > 30_000_000:
                raise ValueError(f'G1 и real отправлены с неверным интервалом: {gap/1e6:.3f} мс')
            if planned == 100_000_000:
                gaps.append(gap/1e6)
            else:
                # The first post-reset lattice can span three refresh slots.
                # Record it, but never count it as an evenly spaced pair.
                transitions.append(gap/1e6)
    if len(gaps) < minimum:
        raise ValueError('Недостаточно равномерно разделённых пар G1/real')
    return {'scope':'WSI handoff, physical display unverified', 'pairs':len(gaps),
            'g1_to_real_ms_min':min(gaps), 'g1_to_real_ms_max':max(gaps),
            'nonuniform_transition_gaps_ms':transitions}


def prepare_gamescope_layer(run):
    """Private explicit alias of the SAME WSI binary, including old loaders.

    Old loaders cannot reorder an implicit-only manifest via INSTANCE_LAYERS.
    Suppress only the original implicit activation using its declared switch;
    explicitly enable a per-run copy without implicit-only environment gates.
    No global manifests, compositor settings or shared libraries are modified.
    """
    if os.environ.get('ENABLE_GAMESCOPE_WSI') != '1':
        return None
    name = 'VK_LAYER_FROG_gamescope_wsi_x86_64'
    roots = []
    for key in ('VK_IMPLICIT_LAYER_PATH','VK_LAYER_PATH','VK_ADD_LAYER_PATH'):
        roots += [Path(p) for p in os.environ.get(key,'').split(':') if p]
    homes = [os.environ.get('XDG_CONFIG_HOME',str(Path.home()/'.config')),
             os.environ.get('XDG_DATA_HOME',str(Path.home()/'.local/share')),
             str(Path.home()/'.config'),str(Path.home()/'.local/share'),
             '/etc','/usr/local/share','/usr/share']
    homes += [p for p in os.environ.get('XDG_CONFIG_DIRS','/etc/xdg').split(':') if p]
    homes += [p for p in os.environ.get('XDG_DATA_DIRS','/usr/local/share:/usr/share').split(':') if p]
    roots += [Path(p)/'vulkan'/kind for p in homes for kind in ('implicit_layer.d','explicit_layer.d')]
    for root in dict.fromkeys(roots):
        for manifest in sorted(root.glob('*.json')):
            try:
                data = json.loads(manifest.read_text())
            except (OSError,ValueError):
                continue
            layer = data.get('layer',{})
            if layer.get('name') != name:
                continue
            disable = layer.get('disable_environment',{})
            if disable != {'DISABLE_GAMESCOPE_WSI':'1'}:
                raise ValueError('Неподдерживаемое описание выключателя Gamescope WSI: '+str(manifest))
            if os.environ.get('DISABLE_GAMESCOPE_WSI') == '1':
                raise ValueError('Gamescope WSI отключён во входном окружении')
            library = layer.get('library_path','')
            if not library:
                raise ValueError('Нет library_path Gamescope WSI')
            if '/' in library and not Path(library).is_absolute():
                library = str((manifest.parent/library).resolve())
            layer['library_path'] = library
            layer.pop('enable_environment',None)
            layer.pop('disable_environment',None)
            directory = run/'explicit-wsi'
            directory.mkdir()
            private = directory/'VkLayer_FROG_gamescope_wsi_x86_64.json'
            private.write_text(json.dumps(data,indent=2)+'\n')
            metadata = {'name':name,'original_manifest':str(manifest),
                        'explicit_manifest':str(private),'library_path':library,
                        'implicit_activation_suppressed':True,'explicit_activation_required':True}
            (run/'GAMESCOPE_LAYER_ORDER.json').write_text(json.dumps(metadata,indent=2)+'\n')
            return {'directory':directory,'disable_environment':disable,'metadata':metadata}
    raise ValueError('Не найден manifest штатного Gamescope WSI x86_64')


def child_env(kit, active, validation, order, preserve_layers=False):
    # Only this child is changed; no global manifests, Steam options or settings.
    env = {k:v for k,v in os.environ.items() if not k.startswith(('GFG_NATIVE_', 'GFG_TEST_', 'LSFGVK_', 'DISABLE_GFG_NATIVE_'))}
    env.pop('VK_INSTANCE_LAYERS', None)
    paths = [str(kit/'layers')]
    if preserve_layers:
        paths += [p for p in os.environ.get('VK_LAYER_PATH','').split(':') if p]
    paths += ['/usr/share/vulkan/explicit_layer.d', '/etc/vulkan/explicit_layer.d']
    for base in (os.environ.get('XDG_DATA_HOME', str(Path.home()/'.local/share')),
                 os.environ.get('XDG_CONFIG_HOME', str(Path.home()/'.config'))):
        paths.append(base+'/vulkan/explicit_layer.d')
    env['VK_LAYER_PATH'] = ':'.join(paths)
    layers = ['VK_LAYER_GFG_native'] if active else []
    if validation:
        layers.insert(0 if order=='validation-first' else len(layers), 'VK_LAYER_KHRONOS_validation')
    if preserve_layers:
        gamescope = 'VK_LAYER_FROG_gamescope_wsi_x86_64'
        layers += [p for p in os.environ.get('VK_INSTANCE_LAYERS','').split(':')
                   if p and p not in ('VK_LAYER_GFG_native','VK_LAYER_KHRONOS_validation')]
        if os.environ.get('ENABLE_GAMESCOPE_WSI') == '1' and gamescope not in layers:
            # An explicit per-run manifest lets old and new loaders honor this
            # ordered list. Otherwise Gamescope runs before every explicit layer
            # and GSF sees its driver-only MAILBOX create chain, not app FIFO.
            # Keep Gamescope enabled; all synthetic presents must pass through it.
            layers.append(gamescope)
    if layers:
        env['VK_INSTANCE_LAYERS'] = ':'.join(layers)
    env.update(GFG_TEST_SOURCE_HZ='5', GFG_TEST_FRAMES='12', GFG_TEST_WIDTH='96', GFG_TEST_HEIGHT='64',
               GFG_TEST_VALIDATION='1' if validation else '0', VK_LOADER_DEBUG='error,warn')
    if active:
        env.update(GFG_NATIVE_FG='1', GFG_NATIVE_GPU_BRIDGE='1', GFG_NATIVE_ASYNC='1',
                   GFG_NATIVE_GPU_SYNC='same-device', GFG_NATIVE_FORCE_FIFO='1', GFG_NATIVE_MODE='2',
                   GFG_NATIVE_REAL_WSI_PROBE='1', GFG_NATIVE_TRACE='1', GFG_NATIVE_TIMING='1',
                   GFG_TEST_EXPECT_GPU='1', GFG_NATIVE_GPU_BRIDGE_VERIFY='1',
                   GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY='2',
                   GFG_TEST_EXPECT_LAYER_PATH=str(kit/'libVkLayer_gfg_native.so'))
    return env


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--noninteractive', action='store_true')
    ap.add_argument('--visual', action='store_true', help='Также показать увеличенное сравнение A/B')
    ap.add_argument('--order', choices=['native-first','validation-first'], default='native-first')
    ap.add_argument('--results', type=Path, default=Path.home()/'GSF-Test-Results')
    args = ap.parse_args()
    args.results.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-'), dir=args.results))
    report = {'status':'FAIL', 'stage':'preflight', 'physical_scanout':'UNVERIFIED', 'cases':{},
              'platform':platform.platform(), 'arch':platform.machine(), 'layer_order':args.order}
    code = 1
    print('GSF — автоматический тестовый кандидат. Система и настройки Steam не изменяются.', flush=True)
    try:
        verify_payload(KIT)
        report['build'] = json.loads((KIT/'BUILD_INFO.json').read_text())
        report['payload_verified'] = True
        report['library_sha256'] = digest(KIT/'libVkLayer_gfg_native.so')
        if platform.machine() != 'x86_64':
            raise ValueError('Требуется Linux x86_64')
        if not os.environ.get('DISPLAY'):
            raise ValueError('Нет XWayland DISPLAY: запустите из рабочего стола Steam Deck')
        # ZIP extraction can discard execute bits. Repair only our verified local ELF.
        (KIT/'wsi-gpu-smoke').chmod((KIT/'wsi-gpu-smoke').stat().st_mode | 0o100)
        for name in ('libVkLayer_gfg_native.so', 'wsi-gpu-smoke'):
            probe = subprocess.run(['ldd', str(KIT/name)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
            (run/(name+'.dependencies.txt')).write_text(probe.stdout)
            if probe.returncode or 'not found' in probe.stdout:
                raise ValueError('Недоступны системные библиотеки для '+name)
        validation = any((Path(p)/'VkLayer_khronos_validation.json').is_file()
                         for p in child_env(KIT, False, False, args.order)['VK_LAYER_PATH'].split(':'))
        report['validation_layer_available'] = validation
        cases = [('baseline',None,False), ('tiles-2x','tiles',False),
                 ('lmc-2x','lmc-gpu',False), ('lmc-mutable-2x','lmc-gpu',False), ('lmc-recreate-resize','lmc-gpu',True),
                 ('lmc-paced-2x','lmc-gpu',False)]
        if args.visual or not args.noninteractive:
            cases += [('A-baseline-visual',None,False), ('B-lmc-visual','lmc-gpu',False)]
        for label, algo, lifecycle in cases:
            visual = label.endswith('-visual')
            paced = label in ('lmc-paced-2x', 'B-lmc-visual')
            frames = 40 if visual else 12
            report['stage'] = label
            print('Проверка: '+label, flush=True)
            env = child_env(KIT, algo is not None, validation, args.order)
            if label=='lmc-mutable-2x':
                env['GFG_TEST_MUTABLE_SWAPCHAIN']='1'
                env['GFG_TEST_PROTON_CHAIN']='1'
            env['GFG_TEST_TITLE'] = 'GSF '+label+' | source 5 Hz'+(' | output 10 Hz' if paced else ' | no generation' if algo is None else '')
            if visual:
                print(('A: исходные 5 кадров/с' if algo is None else 'B: GPU LMC 5 → 10 кадров/с')+' — окно 480×320, 8 секунд движения.', flush=True)
                env.update(GFG_TEST_WIDTH='480', GFG_TEST_HEIGHT='320', GFG_TEST_FRAMES=str(frames))
            if algo:
                env['GFG_NATIVE_ALGO'] = algo
                env['GFG_NATIVE_TELEMETRY'] = str(run/(label+'.jsonl'))
            if paced:
                env.update(GFG_NATIVE_OUTPUT_HZ='10', GFG_TEST_ALLOW_SKIPS='1')
            if lifecycle:
                env.update(GFG_TEST_LIFECYCLE='1', GFG_TEST_FRAMES='36')
            proc = subprocess.run([str(KIT/'wsi-gpu-smoke')], env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, timeout=180)
            log = proc.stdout
            (run/(label+'.log')).write_text(log)
            if label=='lmc-mutable-2x' and 'GSF_MUTABLE_SWAPCHAIN unorm_srgb_alias_view=PASS original_metadata_unchanged=PASS' not in log:
                raise ValueError('Нет проверки изменяемых форматов DXVK: '+log[-1000:])
            if proc.returncode or re.search(r'VUID-|VALIDATION ERROR|Validation Error|UNASSIGNED-.*ERROR',log):
                raise ValueError(f'{label}: ошибка Vulkan или приложения (exit={proc.returncode}), см. журнал')
            if 'GPU bridge WSI smoke PASS' not in log:
                raise ValueError(label+': нет подтверждения завершения')
            if algo is None and 'GPU bridge active:' in log:
                raise ValueError('Генератор неожиданно активен в baseline')
            report['cases'][label] = {'status':'PASS', 'counters':check_active(log,lifecycle,KIT,paced=paced,frames=frames) if algo else None}
            if paced:
                report['cases'][label]['cadence'] = check_cadence(run/(label+'.jsonl'), minimum=frames-4)
        report.update(status='PASS', stage='complete')
        code = 0
    except (Exception, KeyboardInterrupt) as error:
        if isinstance(error, subprocess.TimeoutExpired):
            output = error.stdout or ''
            if isinstance(output, bytes): output = output.decode(errors='replace')
            (run/(report['stage']+'.log')).write_text(output)
        report['error'] = str(error) or 'Тест прерван'
    finally:
        (run/'REPORT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
        summary = ('GSF: '+report['status']+'\nЭтап: '+report['stage']+'\n'+report.get('error','Все контрольные тесты пройдены.')+
                   '\nФизический вывод OLED: НЕ ПОДТВЕРЖДЁН.\n')
        (run/'RESULT.txt').write_text(summary)
        archive = run.with_suffix('.zip')
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
            for p in sorted(run.iterdir()):
                if p.is_file(): z.write(p, run.name+'/'+p.name)
        print(summary+'\nАрхив диагностики: '+str(archive), flush=True)
        if not args.noninteractive:
            if shutil.which('kdialog'):
                subprocess.run(['kdialog','--msgbox', summary+'\nАрхив: '+str(archive)], check=False)
            elif sys.stdin.isatty():
                input('Нажмите Enter, чтобы закрыть окно. ')
    return code


if __name__ == '__main__':
    raise SystemExit(main())

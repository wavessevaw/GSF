# SPDX-License-Identifier: GPL-3.0-or-later
"""GSF Next: user-owned, atomic installation and isolated per-game profiles."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import tempfile
import time
import zipfile

DEFAULTS = dict(enabled=False, multiplier=2, quality='performance', refresh_hz=90,
                max_in_flight=2, extra_images=3, present_mode='auto', diagnostics=False)


def read(path, default=None):
    try:
        return json.loads(Path(path).read_text())
    except FileNotFoundError:
        return default


def atomic(path, obj):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temp = tempfile.mkstemp(prefix='.next-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as out:
            json.dump(obj, out, ensure_ascii=False, indent=2)
            out.write('\n')
        os.replace(temp, path)
    finally:
        Path(temp).unlink(missing_ok=True)


def app_id(value):
    if isinstance(value, bool) or not re.fullmatch(r'[0-9]{1,10}', str(value)) or not 0 < int(value) <= 0xffffffff:
        raise ValueError('Неверный Steam AppID')
    return str(int(value))


def validate(settings):
    p = {**DEFAULTS, **settings}
    limits = dict(multiplier=(0, 2, 3, 4), refresh_hz=(40, 45, 60, 72, 80, 90, 120, 144, 165, 240),
                  max_in_flight=(1, 2, 3), extra_images=(3, 5))
    for key, values in limits.items():
        if type(p[key]) is not int or p[key] not in values:
            raise ValueError('Неверное значение: '+key)
    if p['quality'] not in ('performance', 'quality') or p['present_mode'] not in ('auto', 'keep'):
        raise ValueError('Неверный профиль качества/вывода')
    if type(p['diagnostics']) is not bool: raise ValueError('Неверный режим диагностики')
    if type(p['enabled']) is not bool:
        raise ValueError('Неверное состояние профиля')
    return p


def verify(directory):
    directory = Path(directory)
    checks = read(directory/'CHECKSUMS.json')
    if not isinstance(checks, dict) or not checks or not {'libgsf-next.so', 'gsf-next.json', 'BUILD_INFO.json'} <= checks.keys():
        raise ValueError('Неполный пакет GSF Next')
    for name, expected in checks.items():
        path = directory/name
        if Path(name).is_absolute() or '..' in Path(name).parts or path.is_symlink() or not path.resolve().is_relative_to(directory.resolve()):
            raise ValueError('Недопустимый путь в пакете')
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('Повреждён файл: '+name)
    return read(directory/'BUILD_INFO.json')


def environment(kit, run, inherited, profile):
    p = validate(profile)
    # One generator owns presentation. Disable known implicit competitors only
    # in the child; reject explicit layers and injected upscalers before loading.
    explicit = inherited.get('VK_INSTANCE_LAYERS', '')
    if re.search(r'gfg|gsf|mako|lsfg|afmf', explicit, re.I) or inherited.get('PROTON_USE_OPTISCALER') == '1':
        raise ValueError('Уберите другой генератор из параметров этой игры')
    env = {k:v for k,v in inherited.items() if not k.startswith(('GFG_NATIVE_', 'GSF_NEXT_', 'AFMF_'))}
    env.update(DISABLE_GFG_NATIVE_FG='1', DISABLE_LSFGVK='1', DISABLE_MAKO='1', DISABLE_AFMF='1')
    env['VK_LOADER_LAYERS_DISABLE'] = ','.join(filter(None,[inherited.get('VK_LOADER_LAYERS_DISABLE'), 'VK_LAYER_MAKO_render,VK_LAYER_LS_frame_generation,VK_LAYER_AFMF,VK_LAYER_GFG_native']))
    env['VK_ADD_LAYER_PATH'] = str(kit)+(':'+inherited['VK_ADD_LAYER_PATH'] if inherited.get('VK_ADD_LAYER_PATH') else '')
    # Explicit manifest: no root, global implicit install, or Gamescope layer rewriting.
    # Also support pre-1.3.234 loaders that ignore VK_ADD_LAYER_PATH. Only
    # explicit-layer search is extended; implicit Gamescope remains untouched.
    paths=[str(kit)]
    for key in ('VK_LAYER_PATH','VK_ADD_LAYER_PATH'):
        paths.extend(x for x in inherited.get(key,'').split(':') if x)
    home=Path(inherited.get('HOME',str(Path.home())))
    for base in (inherited.get('XDG_CONFIG_HOME',str(home/'.config')),
                 inherited.get('XDG_DATA_HOME',str(home/'.local/share')),
                 *inherited.get('XDG_CONFIG_DIRS','/etc/xdg').split(':'),
                 *inherited.get('XDG_DATA_DIRS','/usr/local/share:/usr/share').split(':'),
                 '/etc','/usr/share'):
        if base: paths.append(base+'/vulkan/explicit_layer.d')
    env['VK_LAYER_PATH']=':'.join(dict.fromkeys(paths))
    env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_GSF_next' +(':'+explicit if explicit else '')
    env.update(GSF_NEXT_MULTIPLIER=str(p['multiplier']), GSF_NEXT_REFRESH_HZ=str(p['refresh_hz']),
               GSF_NEXT_MAX_IN_FLIGHT=str(p['max_in_flight']), GSF_NEXT_STATUS_DIR=str(run/'status'),
               AFMF_EXTRA_IMAGES=str(p['extra_images']), AFMF_PERFORMANCE_MODE=p['quality'],
               AFMF_SEARCH_MODE='standard', AFMF_MIN_FPS='0', AFMF_GOVERNOR='1',
               AFMF_ASYNC='1', AFMF_PACING='1', AFMF_PROFILE='1', AFMF_LOG='2',
               AFMF_INTERPOLATE='1', AFMF_FAST_MOTION_RESPONSE='blend', AFMF_HUD_DETECT='1',
               AFMF_DIRECT_OUTPUT='0', AFMF_DIRECT_INGEST='1', AFMF_ACQUIRE_TIMEOUT_US='0',
               AFMF_PRESENT_MODE=p['present_mode'], AFMF_GAMESCOPE='1')
    if p['diagnostics']: env['GSF_NEXT_GPU_REPORT']=str(run/'gpu-pairs.jsonl')
    return env


class Runtime:
    def __init__(self, home, plugin):
        self.home, self.plugin = Path(home), Path(plugin)
        self.root = self.home/'.local/share/gsf-next'
        self.results = self.home/'GSF-Next-Results'
        self.samples = {}

    def config(self):
        return read(self.root/'config.json', dict(schema=1, games={}))

    def install(self):
        payload = self.plugin/'payload'
        info = verify(payload)
        build = hashlib.sha256((payload/'CHECKSUMS.json').read_bytes()).hexdigest()
        dest = self.root/'builds'/build
        dest.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        if dest.exists():
            try:
                verify(dest)
                for name, expected in read(payload/'CHECKSUMS.json').items():
                    if name != 'gsf-next.json' and hashlib.sha256((dest/name).read_bytes()).hexdigest() != expected:
                        raise ValueError('Installed build differs from packaged source')
                if read(dest/'gsf-next.json')['layer']['library_path'] != str(dest/'libgsf-next.so'):
                    raise ValueError('Installed layer manifest was redirected')
            except (OSError, ValueError):
                # A corrupt install is replaced, not trusted on directory name alone.
                quarantine = dest.with_name('.corrupt-'+build+'-'+str(time.time_ns()))
                dest.rename(quarantine)
        if not dest.exists():
            stage = Path(tempfile.mkdtemp(prefix='.install-', dir=dest.parent))
            try:
                shutil.copytree(payload, stage, dirs_exist_ok=True)
                verify(stage)
                manifest = read(stage/'gsf-next.json')
                manifest['layer']['library_path'] = str(dest/'libgsf-next.so')
                atomic(stage/'gsf-next.json', manifest)
                # Relocation changes the manifest hash; the installed manifest is
                # verified against its relocated checksum, binary hash stays pinned.
                checks = read(stage/'CHECKSUMS.json')
                checks['gsf-next.json'] = hashlib.sha256((stage/'gsf-next.json').read_bytes()).hexdigest()
                atomic(stage/'CHECKSUMS.json', checks)
                stage.rename(dest)
            finally:
                if stage.exists(): shutil.rmtree(stage)
        for name in ('launch.py', 'next_runtime.py', 'gsf_amdgpu.py'):
            source = self.plugin/name if name == 'launch.py' else self.plugin/'py_modules'/name
            data = source.read_bytes()
            fd, temporary = tempfile.mkstemp(prefix='.launcher-', dir=self.root)
            with os.fdopen(fd, 'wb') as out: out.write(data)
            os.replace(temporary, self.root/name)
        atomic(self.root/'current.json', dict(kit=str(dest), build=info))
        return info

    def selection(self):
        return read(self.root/'selection.json', dict(appid=0))

    def select(self, value):
        selected = dict(appid=int(app_id(value)))
        atomic(self.root/'selection.json', selected)
        return selected

    def options(self, value):
        return 'python3 '+shlex.quote(str(self.root/'launch.py'))+' --appid '+app_id(value)+' -- %command%'

    def configure(self, value, patch):
        aid = app_id(value)
        if not isinstance(patch, dict) or patch.keys()-DEFAULTS.keys():
            raise ValueError('Неизвестный параметр')
        cfg = self.config()
        entry = validate({**cfg['games'].get(aid, {}), **patch})
        if entry['multiplier'] == 0: entry['enabled'] = False
        cfg['games'][aid] = entry
        atomic(self.root/'config.json', cfg)
        return {**entry, 'options':self.options(aid), 'restart_required':True}

    def prepare(self, value):
        self.install()
        aid = app_id(value)
        entry = self.config()['games'].get(aid, {})
        return self.configure(aid, dict(enabled=True, multiplier=entry.get('multiplier') or 2))

    def disable_all(self):
        cfg = self.config()
        for entry in cfg['games'].values(): entry['enabled'] = False
        atomic(self.root/'config.json', cfg)

    def games(self):
        roots = {self.home/'.local/share/Steam/steamapps', self.home/'.steam/steam/steamapps'}
        for root in list(roots):
            try:
                for path in re.findall(r'"path"\s+"([^"]+)"', (root/'libraryfolders.vdf').read_text()):
                    roots.add(Path(path.replace('\\\\', '\\'))/'steamapps')
            except OSError: pass
        games = {}
        for root in roots:
            for manifest in root.glob('appmanifest_*.acf'):
                try:
                    text = manifest.read_text()
                    aid = app_id(re.search(r'"appid"\s+"(\d+)"', text)[1])
                    name = re.search(r'"name"\s+"([^"\n]+)"', text)[1]
                    games[aid] = dict(appid=int(aid), name=name)
                except (ValueError, TypeError, OSError): pass
        return sorted(games.values(), key=lambda g:g['name'].lower())

    def latest(self, value):
        return next((p for p in sorted(self.results.glob('game-'+app_id(value)+'-*'), reverse=True)
                     if p.is_dir() and not p.is_symlink()), None)

    def status(self, value):
        aid = app_id(value)
        profile = validate(self.config()['games'].get(aid, {}))
        run = self.latest(aid)
        report = read(run/'REPORT.json', {}) if run else {}
        live = []
        if run and report.get('stage') == 'running':
            for path in (run/'status').glob('*.json'):
                try:
                    r = read(path)
                    if not 0 <= time.monotonic_ns()-r['monotonic_ns'] < 3_000_000_000: continue
                    old = self.samples.get(str(path))
                    if old and any(r[k]<old.get(k,0) for k in ('accepted_real','accepted_synthetic','generated')): old=None
                    self.samples[str(path)] = r.copy()
                    r.update(generated_fps=None, output_fps=None, effective_multiplier=None)
                    if old and r['monotonic_ns'] > old['monotonic_ns']:
                        seconds=(r['monotonic_ns']-old['monotonic_ns'])/1e9
                        real=r['accepted_real']-old['accepted_real']
                        generated=r['generated']-old['generated']
                        accepted=r['accepted_synthetic']-old['accepted_synthetic']
                        r.update(generated_fps=generated/seconds, output_fps=(real+accepted)/seconds,
                                 effective_multiplier=(real+accepted)/real if real else None)
                    elif old:
                        r.update({k:old.get(k) for k in ('generated_fps','output_fps','effective_multiplier')})
                    self.samples[str(path)]=r.copy()
                    live.append(r)
                except (OSError, ValueError, KeyError, TypeError): pass
        reason=None
        if run and not live:
            try:
                with (run/'game.log').open('rb') as log:
                    log.seek(0,2);log.seek(max(0,log.tell()-65536))
                    tail=log.read().decode(errors='replace')
                matches=re.findall(r'(?:pass-through \(([^\n]+)\)|generation resources failed[^\n]*|no compute queue[^\n]*)',tail)
                if matches: reason=next((m for m in reversed(matches) if m),'GPU interpolation unavailable')
            except OSError: pass
        return dict(profile=profile, options=self.options(aid), live=live, report=report,reason=reason,
                    state='generating' if any(r.get('accepted_synthetic',0)>0 for r in live) else 'capturing' if live else 'blocked' if reason else 'waiting',
                    build=read(self.root/'current.json', {}).get('build'))

    def export(self, value):
        run=self.latest(value)
        if not run: raise ValueError('Игра ещё не запускалась через GSF Next')
        dest=self.results/('GSF-Next-'+app_id(value)+'.zip')
        with zipfile.ZipFile(dest.with_suffix('.tmp'), 'w', zipfile.ZIP_DEFLATED) as z:
            for path in run.rglob('*'):
                if path.is_file() and not path.is_symlink(): z.write(path,path.relative_to(run))
        dest.with_suffix('.tmp').replace(dest)
        return dict(path=str(dest))

"""GSF test runtime. User files only; no Steam database or system mutations."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import tempfile
import time
import zipfile


def load_json(path, default=None):
    try:
        return json.loads(Path(path).read_text())
    except FileNotFoundError:
        return default


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temp = tempfile.mkstemp(prefix='.gsf-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as f:
            json.dump(value, f, ensure_ascii=False, indent=2)
            f.write('\n')
        os.replace(temp, path)
    finally:
        Path(temp).unlink(missing_ok=True)


def app_id(value):
    if isinstance(value, bool) or not re.fullmatch(r'[0-9]{1,10}', str(value)):
        raise ValueError('Неверный AppID')
    number = int(value)
    if not 0 < number <= 0xffffffff:
        raise ValueError('Неверный AppID')
    return str(number)


def candidate_module(kit):
    spec = importlib.util.spec_from_file_location('gsf_candidate_runtime', Path(kit)/'run-test.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def launch_options(root, appid, old):
    appid = app_id(appid)
    if not isinstance(old, str) or len(old) > 16000:
        raise ValueError('Неверные параметры запуска')
    if re.search(r'gsf-test|gfg|mako|lsfg|lossless', old, re.I):
        raise ValueError('В запуске уже есть GSF/GFG/другой генератор. Сначала уберите его для отдельного теста GSF.')
    wrapper = 'python3 '+shlex.quote(str(Path(root)/'launch.py'))+' --appid '+appid+' -- '
    if old.count('%command%') > 1:
        raise ValueError('Несколько %command%: автоматическое изменение неоднозначно')
    if re.search(r'''["']%command%["']''', old):
        raise ValueError('Уберите кавычки вокруг %command% перед автоматическим подключением')
    return old.replace('%command%', wrapper+'%command%') if '%command%' in old else wrapper+'%command%'+(' '+old if old else '')


PID_CACHE = {}


def process_matches(record):
    try:
        pid = int(record['pid'])
        if pid <= 0:
            return False
        ticks = int(record['process_start_ticks'])
        key = (pid, ticks)
        # Proton can have a different PID namespace from Decky's backend.
        # Match both start time and NSpid; never trust a recycled numeric PID.
        direct = Path('/proc')/str(PID_CACHE.get(key, pid))
        candidates = [direct]
        candidates.extend(p for p in Path('/proc').iterdir() if p.name.isdigit() and p != direct)
        for directory in candidates:
            try:
                fields = (directory/'stat').read_text().rsplit(')', 1)[1].split()
                if int(fields[19]) != ticks or fields[0] == 'Z':
                    continue
                if directory.name != str(pid):
                    ns = re.search(r'^NSpid:\s+(.+)$', (directory/'status').read_text(), re.M)
                    if not ns or str(pid) not in ns[1].split():
                        continue
                if len(PID_CACHE) > 128:
                    PID_CACHE.clear()
                PID_CACHE[key] = int(directory.name)
                return True
            except (OSError, IndexError, ValueError):
                continue
        return False
    except (ValueError, KeyError, OSError, IndexError):
        return False


def live_records(run):
    records = []
    for p in (Path(run)/'status').glob('*.json'):
        try:
            r = load_json(p)
            age = time.time()*1000-float(r['updated_unix_ms'])
            if -1000 <= age < 3000 and process_matches(r):
                records.append(r)
        except (ValueError, TypeError, KeyError, OSError):
            continue
    return records


def gate_rejection(run):
    try:
        with (Path(run)/'game.log').open('rb') as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell()-65536))
            tail = f.read().decode(errors='replace')
        rows = re.findall(r'^GSF_REAL_WSI_GATE .*candidate=(\d+) reason=(\S+)', tail, re.M)
        return rows[-1][1] if rows and rows[-1][0] == '0' else None
    except OSError:
        return None


class Runtime:
    def __init__(self, home, plugin):
        self.home, self.plugin = Path(home), Path(plugin)
        self.root = self.home/'.local/share/gsf-test'
        self.results = self.home/'GSF-Test-Results'

    def config(self):
        return load_json(self.root/'config.json', {'schema': 1, 'games': {}})

    def selection(self):
        saved = load_json(self.root/'selection.json', {})
        return {'appid': int(app_id(saved['appid'])) if saved.get('appid') else 0}

    def select(self, appid):
        selected = {'appid': int(app_id(appid))}
        atomic_json(self.root/'selection.json', selected)
        return selected

    def install(self):
        kit = self.plugin/'payload'
        base = candidate_module(kit)
        base.verify_payload(kit)
        build = load_json(kit/'BUILD_INFO.json')['source_commit']
        if not re.fullmatch('[0-9a-f]{40}', build):
            raise ValueError('Неверная версия GPU слоя')
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        dest = self.root/'builds'/build
        dest.parent.mkdir(exist_ok=True)
        if not dest.exists():
            stage = Path(tempfile.mkdtemp(prefix='.install-', dir=dest.parent))
            try:
                for name in (*base.PAYLOAD, 'CHECKSUMS.json'):
                    out = stage/name
                    out.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(kit/name, out)
                base.verify_payload(stage)
                stage.rename(dest)
            finally:
                if stage.exists():
                    shutil.rmtree(stage)
        base.verify_payload(dest)
        for source, name in ((self.plugin/'launch.py', 'launch.py'),
                             (self.plugin/'py_modules/gsf_runtime.py', 'gsf_runtime.py'),
                             (self.plugin/'py_modules/gsf_diagnostic.py', 'gsf_diagnostic.py'),
                             (self.plugin/'py_modules/gsf_segments.py', 'gsf_segments.py'),
                             (self.plugin/'py_modules/gsf_amdgpu.py', 'gsf_amdgpu.py')):
            temp = self.root/(name+'.new')
            shutil.copyfile(source, temp)
            temp.chmod(0o600)
            os.replace(temp, self.root/name)
        atomic_json(self.root/'current.json', {'build': build, 'kit': str(dest)})
        return {'build': build, 'launcher': str(self.root/'launch.py')}

    def prepare(self, appid, original):
        appid = app_id(appid)
        info = self.install()
        cfg = self.config()
        saved = cfg['games'].get(appid)
        if saved and original == saved['expected']:
            return {'options': saved['expected'], 'original': saved['original'], **info}
        if saved and saved.get('configured'):
            raise ValueError('Параметры запуска изменены после GSF. Сначала восстановите/отключите прежний профиль.')
        expected = launch_options(self.root, appid, original)
        cfg['games'][appid] = {'original': original, 'expected': expected, 'enabled': False,
                               'configured': False, 'multiplier': 2, 'updated': time.time()}
        atomic_json(self.root/'config.json', cfg)
        return {'options': expected, 'original': original, **info}

    def confirm(self, appid, actual):
        cfg = self.config()
        saved = cfg['games'][app_id(appid)]
        if actual != saved['expected']:
            raise ValueError('Steam не подтвердил параметры запуска: GSF не включён')
        saved.update(enabled=True, configured=True, manual_unverified=False)
        atomic_json(self.root/'config.json', cfg)
        return {'enabled': True, 'restart_required': True}

    def disable(self, appid, actual):
        cfg = self.config()
        saved = cfg['games'].get(app_id(appid))
        if not saved:
            return {'options': actual, 'restore': False}
        saved['enabled'] = False
        atomic_json(self.root/'config.json', cfg)
        if actual != saved['expected']:
            return {'options': actual, 'restore': False, 'message': 'GSF отключён. Изменённые вами параметры запуска сохранены.'}
        return {'options': saved['original'], 'restore': True}

    def forget(self, appid, actual):
        cfg = self.config()
        saved = cfg['games'].get(app_id(appid))
        if saved and not saved['enabled'] and actual == saved['original']:
            del cfg['games'][app_id(appid)]
            atomic_json(self.root/'config.json', cfg)

    def prepare_manual_game(self, appid):
        """Install verified payload and return a copyable option WITHOUT claiming a Steam write.
        If Steam could not be read, a new game's original options are unknown and
        must be preserved by the user; never silently write an empty backup to Steam.
        """
        aid = app_id(appid)
        existing = self.config()['games'].get(aid)
        if existing and existing.get('expected'):
            self.install()
            return {'options': existing['expected'],
                    'warning': 'Проверьте существующие параметры запуска Steam: они могли быть изменены после последнего сохранения GSF.'}
        prepared = self.prepare(aid, '')
        return {'options': prepared['options'],
                'warning': 'Шаблон рассчитан на пустые параметры запуска. Если там уже есть аргументы, сохраните их и добавьте GSF перед %command%, не стирая пользовательские настройки.'}

    def confirm_manual_game(self, appid, options):
        """Only after an explicit user's manual-paste acknowledgment; not Steam verification."""
        cfg = self.config()
        entry = cfg['games'].get(app_id(appid))
        if not entry or entry.get('expected') != options:
            raise ValueError('Ручная команда не совпадает с подготовленной; повторите подготовку')
        # A fresh installation must still be present before enabling a manual profile.
        current = load_json(self.root/'current.json', {})
        if not current.get('kit') or not Path(current['kit']).is_dir():
            raise ValueError('Пакет GSF не установлен; сначала подготовьте команду')
        entry.update(enabled=True, configured=True, manual_unverified=True)
        atomic_json(self.root/'config.json', cfg)
        return {'enabled': True, 'steam_verified': False, 'restart_required': True}

    def disable_manual_game(self, appid):
        """Safe bypass even when Steam's details API is unavailable; never rewrite user settings."""
        cfg = self.config()
        entry = cfg['games'].get(app_id(appid))
        if entry:
            entry['enabled'] = False
            atomic_json(self.root/'config.json', cfg)
        return {'enabled': False, 'steam_options_untouched': True}

    def set_diagnostic_mode(self, appid, mode):
        if mode not in ('normal', 'normal-no-shadow', 'normal-adaptive-pairs', 'normal-queue-observe', 'normal-no-watch', 'normal-no-calibration', 'normal-calibration-every-frame', 'normal-no-latency-admission', 'source-only', 'compute-only', 'matched-compute'):
            raise ValueError('Неверный диагностический режим')
        aid = app_id(appid)
        cfg = self.config()
        entry = cfg['games'].get(aid)
        if not entry:
            raise ValueError('Сначала настройте выбранную игру')
        if mode == 'normal-adaptive-pairs' and entry.get('multiplier', 2) == 3:
            raise ValueError('Адаптивная пара доступна только для ×2')
        entry['diagnostic_mode'] = mode
        atomic_json(self.root/'config.json', cfg)
        return {'diagnostic_mode': mode, 'restart_required': True}

    def set_multiplier(self, appid, multiplier):
        if type(multiplier) is not int or multiplier not in (2, 3):
            raise ValueError('Множитель генерации должен быть ×2 или ×3')
        aid = app_id(appid)
        cfg = self.config()
        entry = cfg['games'].get(aid)
        if not entry:
            raise ValueError('Сначала настройте выбранную игру')
        if multiplier == 3 and entry.get('diagnostic_mode') == 'normal-adaptive-pairs':
            raise ValueError('Адаптивная пара доступна только для ×2. Сначала выберите обычный режим.')
        entry['multiplier'] = multiplier
        atomic_json(self.root/'config.json', cfg)
        return {'multiplier': multiplier, 'restart_required': True}

    def disable_all(self):
        cfg = self.config()
        for saved in cfg['games'].values():
            saved['enabled'] = False
        atomic_json(self.root/'config.json', cfg)
        # Keep the tiny stable bypass: Steam may still reference it after uninstall.

    def games(self):
        roots = {self.home/'.local/share/Steam/steamapps', self.home/'.steam/steam/steamapps'}
        for root in list(roots):
            try:
                text = (root/'libraryfolders.vdf').read_text()
                for path in re.findall(r'"path"\s+"([^"]+)"', text):
                    roots.add(Path(path.replace('\\\\', '\\'))/'steamapps')
            except OSError:
                pass
        games = {}
        for root in roots:
            for manifest in root.glob('appmanifest_*.acf'):
                try:
                    text = manifest.read_text()
                    aid = app_id(re.search(r'"appid"\s+"(\d+)"', text)[1])
                    name = re.search(r'"name"\s+"([^"\n]+)"', text)[1]
                    games[aid] = {'appid': int(aid), 'name': name}
                except (ValueError, TypeError, OSError):
                    continue
        return sorted(games.values(), key=lambda g: g['name'].lower())

    def latest(self, appid):
        appid = app_id(appid)
        runs = sorted(self.results.glob('game-'+appid+'-*'), reverse=True)
        return next((p for p in runs if p.is_dir() and not p.is_symlink()), None)

    def status(self, appid):
        appid = app_id(appid)
        saved = self.config()['games'].get(appid, {})
        run = self.latest(appid)
        records = live_records(run) if run else []
        active = [r for r in records if r.get('generated_wsi_accepted', 0) > 0]
        blocked = gate_rejection(run) if run else None
        return {'enabled': saved.get('enabled', False), 'configured': saved.get('configured', False),
                'launch_options': saved.get('expected', ''),
                'diagnostic_mode': saved.get('diagnostic_mode', 'normal'),
                'manual_unverified': saved.get('manual_unverified', False),
                'multiplier': saved.get('multiplier', 2),
                'state': 'generating' if active else 'bridge' if records else 'blocked' if blocked else 'waiting',
                'gate_rejection': blocked,
                'live': records, 'report': load_json(run/'REPORT.json') if run else None,
                'session': str(run) if run else None}

    def export(self, appid):
        run = self.latest(appid)
        if not run:
            raise ValueError('Ещё нет игрового прогона')
        path = self.results/'GSF-Game-Latest.zip'
        fd, temp = tempfile.mkstemp(prefix='.export-', dir=self.results)
        os.close(fd)
        temporary = Path(temp)
        with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as z:
            from gsf_segments import export_session
            export_session(z,run)
        os.replace(temporary, path)
        return {'path': str(path), 'session': str(run), 'physical_scanout': 'UNVERIFIED'}

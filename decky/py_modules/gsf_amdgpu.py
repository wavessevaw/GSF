"""Passive Steam Deck AMDGPU diagnostics. Global GPU activity, NOT per-game load."""
import json
from pathlib import Path
import re
import threading
import time


def find_amdgpu(root=Path('/sys/class/drm')):
    matches = []
    for card in sorted(root.glob('card[0-9]*')):
        driver = card/'device/driver'
        try:
            if driver.resolve().name != 'amdgpu':
                continue
        except OSError:
            continue
        matches.append(card/'device')
    return matches[0] if len(matches) == 1 else None


def _read_number(path):
    try:
        value = path.read_text().strip()
        return int(value) if value else None
    except (OSError, ValueError):
        return None


def _clock_mhz(path):
    try:
        lines = path.read_text().splitlines()
    except OSError:
        return None
    for line in lines:
        if '*' in line:
            match = re.search(r'(\d+(?:\.\d+)?)\s*[Mm][Hh][Zz]', line)
            if match:
                return float(match.group(1))
    return None


def sample(device, now_ns=None):
    now_ns = time.monotonic_ns() if now_ns is None else int(now_ns)
    result = {'schema': 1, 'event': 'amdgpu_sample', 'sample_ns': now_ns,
              'scope': 'global_gpu_not_game', 'status': 'available'}
    if device is None:
        result.update(status='unavailable', gpu_busy_percent=None,
                      gpu_clock_mhz=None, power_w=None, temperature_c=None)
        return result
    busy = _read_number(device/'gpu_busy_percent')
    if busy is not None and not 0 <= busy <= 100:
        busy = None
    mhz = _clock_mhz(device/'pp_dpm_sclk')
    hwmons = sorted((device/'hwmon').glob('hwmon*'))
    power = temp = None
    for hwmon in hwmons:
        if power is None:
            micro_watts = _read_number(hwmon/'power1_average')
            power = round(micro_watts / 1e6, 3) if micro_watts is not None else None
        if temp is None:
            milli_c = _read_number(hwmon/'temp1_input')
            temp = round(milli_c / 1000, 2) if milli_c is not None else None
    result.update(gpu_busy_percent=busy, gpu_clock_mhz=mhz,
                  power_w=power, temperature_c=temp)
    if all(result[x] is None for x in ('gpu_busy_percent', 'gpu_clock_mhz', 'power_w', 'temperature_c')):
        result['status'] = 'unavailable'
    return result


class GpuSampler:
    def __init__(self, output, root=Path('/sys/class/drm'), interval_seconds=0.1):
        self.path = Path(output)
        self.device = find_amdgpu(root)
        self.interval_seconds = max(0.05, float(interval_seconds))
        self.stop_event = threading.Event()
        self.worker = None

    def start(self):
        if self.worker:
            return
        self.worker = threading.Thread(target=self._run, name='gsf-amdgpu-sampler', daemon=True)
        self.worker.start()

    def stop(self):
        if self.worker:
            self.stop_event.set()
            self.worker.join(timeout=2)
            self.worker = None

    def _run(self):
        try:
            with self.path.open('a', encoding='utf-8') as file:
                if self.device is None:
                    file.write(json.dumps(sample(None), separators=(',', ':')) + '\n')
                    return
                while True:
                    file.write(json.dumps(sample(self.device), separators=(',', ':')) + '\n')
                    file.flush()
                    if self.stop_event.wait(self.interval_seconds):
                        break
        except OSError:
            # Sysfs and diagnostics must NEVER prevent original game execution.
            pass

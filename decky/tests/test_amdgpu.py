import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1]/'py_modules/gsf_amdgpu.py'
spec = importlib.util.spec_from_file_location('gsf_amdgpu', MODULE)
gpu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpu)


class GpuSysfsTests(unittest.TestCase):
    def test_missing_sensors_unavailable(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.assertIsNone(gpu.find_amdgpu(root))
            self.assertEqual(gpu.sample(None)['status'], 'unavailable')

    def test_global_sensors_and_units(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root/'card0/device/hwmon/hwmon0').mkdir(parents=True)
            (root/'card0/device/driver').symlink_to(root/'amdgpu', target_is_directory=True)
            (root/'amdgpu').mkdir()
            dev = root/'card0/device'
            (dev/'gpu_busy_percent').write_text('77\n')
            (dev/'pp_dpm_sclk').write_text('0: 400Mhz\n1: 1600Mhz *\n')
            (dev/'hwmon/hwmon0/power1_average').write_text('9500000\n')
            (dev/'hwmon/hwmon0/temp1_input').write_text('71000\n')
            self.assertEqual(gpu.find_amdgpu(root), dev)
            m = gpu.sample(dev, now_ns=123456)
            self.assertEqual((m['gpu_busy_percent'], m['gpu_clock_mhz'], m['power_w'], m['temperature_c']), (77,1600.0,9.5,71.0))
            self.assertEqual(m['scope'], 'global_gpu_not_game')
            sampler = gpu.GpuSampler(root/'metrics.jsonl', root, .05)
            sampler.start()
            sampler.stop()
            self.assertEqual(json.loads((root/'metrics.jsonl').read_text().splitlines()[0])['event'], 'amdgpu_sample')

    def test_ambiguous_device_is_unavailable(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root/'amdgpu').mkdir()
            for n in (0,1):
                (root/f'card{n}/device').mkdir(parents=True)
                (root/f'card{n}/device/driver').symlink_to(root/'amdgpu', target_is_directory=True)
            self.assertIsNone(gpu.find_amdgpu(root))


if __name__ == '__main__':
    unittest.main()

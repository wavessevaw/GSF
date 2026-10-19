#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual Vulkan packed10 proof + historical bad-dispatch negative control.

No Gamescope, Proton or scanout claim. Requires built Temporal Engine/native tests.
The intentionally bad object exists only in a temporary directory, never runtime.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(a):
    root = Path(__file__).resolve().parents[1]
    source = root/'engine/gfg-temporal/src'
    build = root/a.temporal_build
    tests = root/'engine/gfg-native/tests'
    env = dict(os.environ, GFG_TEST_WRAPPED_PHYSICAL='1')
    a.out.mkdir(parents=True, exist_ok=True)
    result = {'scope': 'ACTUAL_SOFTWARE_VULKAN_OPAQUE_PHYSICAL_NOT_GAMESCOPE_OR_DECK', 'status': 'FAIL'}
    try:
        for name in ('borrowed-device', 'hdr-precision'):
            p = subprocess.run([str(root/a.native_build/name)], env=env, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120, preexec_fn=no_core)
            (a.out/(name+'.log')).write_text(p.stdout)
            assert p.returncode == 0 and 'WRAPPED_PHYSICAL_DISPATCH_PASS' in p.stdout, (name, p.stdout[-2000:])
        original = (source/'te_engine.c').read_text()
        good = '''PFN_vkGetPhysicalDeviceFormatProperties query = dev->borrowed
        ? dev->physical_format_properties : vkGetPhysicalDeviceFormatProperties;'''
        assert original.count(good) == 1, 'Negative-control mutation no longer matches the audited defect'
        with tempfile.TemporaryDirectory(prefix='gsf-bad-dispatch-') as directory:
            temp = Path(directory)
            broken = temp/'te_engine.c'
            broken.write_text(original.replace(good, 'PFN_vkGetPhysicalDeviceFormatProperties query = vkGetPhysicalDeviceFormatProperties;'))
            flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC',
                     '-I'+str(a.vulkan_headers), '-I'+str(source), '-I'+str(build)]
            subprocess.run(['cc', *flags, '-c', str(broken), '-o', str(temp/'engine.o')], check=True)
            exe = temp/'hdr-bad-dispatch'
            subprocess.run(['cc', *flags, str(tests/'hdr_precision.c'), str(temp/'engine.o'),
                            str(build/'pic/te_scheduler.o'), str(build/'pic/te_lmc_fit.o'),
                            '-lvulkan', '-ldl', '-lpthread', '-lm', '-o', str(exe)], check=True)
            p = subprocess.run([str(exe)], env=env, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=120, preexec_fn=no_core)
            (a.out/'historical-bad-dispatch.log').write_text(p.stdout)
            assert p.returncode in (-signal.SIGABRT, -signal.SIGSEGV), (p.returncode, p.stdout)
            assert 'WRAPPED_PHYSICAL_BORROW_SETUP complete' in p.stdout, p.stdout
            assert 'WRAPPED_PHYSICAL_DISPATCH_PASS' not in p.stdout, p.stdout
            result.update(negative_control_returncode=p.returncode, mutation='format_ok uses global loader trampoline',
                          status='PASS')
    finally:
        (a.out/'RESULT.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--temporal-build', type=Path, default=Path('engine/gfg-temporal/build'))
    p.add_argument('--native-build', type=Path, default=Path('engine/gfg-native/build'))
    p.add_argument('--vulkan-headers', type=Path, default=Path('/usr/include'))
    p.add_argument('--out', type=Path, required=True)
    run(p.parse_args())

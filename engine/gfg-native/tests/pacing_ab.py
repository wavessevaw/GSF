#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real Vulkan A/B attribution; software GPU timings are not Deck predictions."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess


def numbers(line):
    return {k: float(v) for k, v in re.findall(r'(\w+)=(-?\d+(?:\.\d+)?)', line)}


def run(kit, output, before_layer=None, repetitions=2):
    output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('candidate', kit/'run-test.py')
    candidate = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(candidate)
    candidate.verify_payload(kit)
    kits = {'current': kit}
    if before_layer:
        old = output/'before-kit'
        (old/'layers').mkdir(parents=True)
        shutil.copy2(before_layer, old/'libVkLayer_gfg_native.so')
        shutil.copy2(kit/'layers/VkLayer_gfg_native.json', old/'layers/VkLayer_gfg_native.json')
        shutil.copy2(kit/'wsi-gpu-smoke', old/'wsi-gpu-smoke')
        kits['before'] = old
    modes = [('current', m) for m in ('baseline', 'source-only', 'compute-only', 'full')]
    if before_layer:
        modes.append(('before', 'full'))
    results = []
    for repeat in range(repetitions):
        cases = [(version, mode, profile, detail) for version, mode in modes
                 for profile in ('45hz', 'variable') for detail in (False, True)]
        if repeat % 2:
            cases.reverse()  # counterbalance warmup/order
        for version, mode, profile, detail in cases:
            name = f'{version}-{mode}-{profile}-detail{int(detail)}-r{repeat}'
            active = mode != 'baseline'
            env = candidate.child_env(kits[version], active, True, 'native-first')
            env.update(GFG_TEST_FRAMES='60', GFG_TEST_SOURCE_HZ='45', GFG_TEST_ABSOLUTE_PACING='1',
                       GFG_TEST_VARIABLE_SOURCE='1' if profile == 'variable' else '0',
                       GFG_TEST_ALLOW_SKIPS='1', GFG_TEST_MUTABLE_SWAPCHAIN='1')
            for key in ('GFG_NATIVE_GPU_BRIDGE_VERIFY', 'GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY',
                        'GFG_NATIVE_TRACE', 'GFG_NATIVE_REAL_WSI_PROBE'):
                env.pop(key, None)
            if active:
                env.update(GFG_NATIVE_ALGO='lmc-gpu', GFG_NATIVE_PACING='1')
                if mode in ('source-only', 'compute-only'):
                    env['GFG_NATIVE_DIAGNOSTIC_MODE'] = mode
                if detail:
                    env['GFG_NATIVE_TELEMETRY'] = str(output/(name+'.jsonl'))
            app = kits[version]/'wsi-gpu-smoke'
            app.chmod(0o755)
            proc = subprocess.run([str(app)], env=env, text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=45)
            log = proc.stdout
            (output/(name+'.log')).write_text(log)
            assert proc.returncode == 0, (name, log[-6000:])
            assert 'validation errors=0' in log, (name, log[-6000:])
            rate = numbers(next(l for l in log.splitlines() if l.startswith('GSF_SOURCE_RATE ')))
            latency = numbers(next(l for l in log.splitlines() if l.startswith('GSF_API_LATENCY ')))
            item = dict(name=name, version=version, mode=mode, profile=profile, detail=detail,
                        repeat=repeat, rate=rate, latency=latency)
            if active:
                stats = numbers(next(l for l in log.splitlines() if l.startswith('GFG native: gpu stats ')))
                assert stats['real'] == 60 and stats['errors'] == stats['present_path_host_pixel_reads'] == 0
                assert stats['synthetic'] + stats['skipped'] == 59
                worker = next(l for l in log.splitlines() if 'output-worker retired' in l)
                item.update(stats=stats, worker=numbers(worker))
                if version == 'current':
                    assert item['worker']['backpressure'] == 0, worker
                if mode in ('source-only', 'compute-only'):
                    assert stats['synthetic'] == 0
                    count = numbers(next(l for l in log.splitlines() if 'diagnostic_retired' in l))['dispatched_outputs']
                    assert (count == 0 if mode == 'source-only' else count > 0)
                    item['diagnostic_dispatched'] = count
                if detail:
                    events = [json.loads(l) for l in (output/(name+'.jsonl')).read_text().splitlines()]
                    pairs = [d for d in events if d.get('event') == 'pair']
                    assert len(pairs) == 60
                    item['source_backlog_pairs'] = sum('source-backlog' in d['drop_detail'] for d in pairs)
                    item['readiness_deadline_pairs'] = sum('gpu-readiness-deadline' in d['drop_detail'] for d in pairs)
            results.append(item)
            print(f"AB {name} source_hz={rate['steady_hz']:.2f} present_p95_ms={latency['present_p95_ms']:.3f}", flush=True)
            (output/'runs.json').write_text(json.dumps(results, indent=2)+'\n')
    summary = []
    for version, mode in modes:
        for profile in ('45hz', 'variable'):
            for detail in (False, True):
                group = [r for r in results if (r['version'], r['mode'], r['profile'], r['detail']) ==
                         (version, mode, profile, detail)]
                summary.append(dict(version=version, mode=mode, profile=profile, detail=detail,
                    steady_hz_median=statistics.median(r['rate']['steady_hz'] for r in group),
                    present_p95_ms_median=statistics.median(r['latency']['present_p95_ms'] for r in group),
                    acquire_p95_ms_median=statistics.median(r['latency']['acquire_p95_ms'] for r in group)))
    guards=[]
    for row in summary:
        if row['version']!='current' or row['mode']!='full':
            continue
        group=[r for r in results if r['version']=='current' and r['mode']=='full' and
               r['profile']==row['profile'] and r['detail']==row['detail']]
        generated=sum(r['stats']['synthetic'] for r in group)
        real=sum(r['stats']['real'] for r in group)
        baseline=next(b for b in summary if b['mode']=='baseline' and
                      b['profile']==row['profile'] and b['detail']==row['detail'])
        ratio=row['steady_hz_median']/baseline['steady_hz_median']
        # A near-zero-output fallback must not repeat the observed ~20/45
        # source-rate collapse. Meaningful generation has a separate GPU and
        # display budget, exercised by the strict slow-source pixel tests.
        guards.append(dict(profile=row['profile'],detail=row['detail'],generated=generated,
                           real=real,source_ratio=ratio,checked=generated<=real*.05,
                           passed=generated>real*.05 or ratio>=.85))
    (output/'summary.json').write_text(json.dumps(dict(
        source_commit=json.loads((kit/'BUILD_INFO.json').read_text())['source_commit'],
        before_commit='61037766aef41703668d8029a54303e5f1e0da71' if before_layer else None,
        physical_scanout='UNVERIFIED', steam_deck_performance='UNVERIFIED', rows=summary,
        near_zero_output_fallback_guards=guards), indent=2)+'\n')
    assert all(g['passed'] for g in guards), guards
    print(f'PACING_AB_PASS runs={len(results)} preserved_real_frames=60 validation_errors=0 hardware_prediction=0')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--before-layer', type=Path)
    parser.add_argument('--repetitions', type=int, default=2)
    args = parser.parse_args()
    run(args.kit.resolve(), args.output.resolve(), args.before_layer.resolve() if args.before_layer else None, args.repetitions)

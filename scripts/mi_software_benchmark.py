#!/usr/bin/env python3
"""Deterministic Vulkan comparison; never presents software timing as Deck data."""
import argparse
import json
import math
from pathlib import Path
import statistics
import struct
import subprocess


def sequence(path, width, height, count):
    header = repr({'descr': '|u1', 'fortran_order': False, 'shape': (count, height, width, 3)})
    header += ' ' * ((-len(header)-11) % 16) + '\n'
    with path.open('wb') as out:
        out.write(b'\x93NUMPY\x01\x00' + struct.pack('<H', len(header)) + header.encode())
        for frame in range(count):
            pixels = bytearray()
            for y in range(height):
                for x in range(width):
                    u, v = x-frame*3, y-frame
                    if x < 48 and y < 12:  # static high-contrast HUD
                        c = 230 if (x//3)%2 else 20
                        pixels.extend((c, c, c))
                    else:
                        pixels.extend((int(110+65*math.sin(u*.09)*math.cos(v*.06)),
                                       int(125+55*math.sin(v*.08+u*.03)),
                                       int(120+60*math.cos(u*.07-v*.05))))
            out.write(pixels)


def percentiles(values):
    values = sorted(values)
    def at(p):
        return values[min(len(values)-1, math.ceil(p*len(values))-1)] if values else None
    return {'p50': at(.5), 'p95': at(.95), 'p99': at(.99)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--pairs', type=int, default=8)
    parser.add_argument('--width', type=int, default=256)
    parser.add_argument('--height', type=int, default=160)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    frames = args.out/'pan.npy'
    sequence(frames, args.width, args.height, args.pairs*4+1)
    results = []
    for algo in ('lmc-gpu', 'shared-flow', 'flow-lite1', 'flow-lite2'):
        for outputs in (1, 2, 3):
            for batch in ((0, 1) if algo == 'flow-lite2' else (0,)):
                name = f'{algo}-x{outputs+1}-batch{batch}'
                command = ['engine/gfg-temporal/build/te-bench', '--frames', str(frames),
                           '--pairs', str(args.pairs), '--step', '4', '--outputs', str(outputs),
                           '--algo', algo, '--tier', '2', '--batch', str(batch),
                           '--uniform-phases', '--json', str(args.out/(name+'.jsonl')), '--validation', '--no-download']
                with (args.out/(name+'.log')).open('w') as log:
                    run = subprocess.run(command, stdout=log, stderr=log, timeout=180)
                if run.returncode:
                    raise RuntimeError(f'{name}: exit {run.returncode}')
                rows = [json.loads(line) for line in (args.out/(name+'.jsonl')).read_text().splitlines()]
                warm = rows[1:]
                pair = [r['analysis_ms']+sum(f['total_ms'] for f in r['frames']) for r in warm]
                row = {'algorithm': algo, 'multiplier': outputs+1, 'batch': bool(batch),
                       'resolution': [args.width, args.height], 'samples': len(warm),
                       'phases': [frame['t'] for frame in rows[-1]['frames']],
                       'analysis_ms': percentiles([r['analysis_ms'] for r in warm]),
                       'gpu_pair_ms': percentiles(pair),
                       'gpu_per_synthetic_ms': percentiles([v/outputs for v in pair]),
                       'render_ms': [percentiles([r['frames'][i]['total_ms'] for r in warm]) for i in range(outputs)],
                       'allocation_bytes': rows[-1]['allocation_bytes'],
                       'source_fps_degradation': None, 'bandwidth_bytes_per_second': None,
                       'power_watts': None, 'hardware': 'llvmpipe/software; NOT Steam Deck',
                       'p95_p99_note': 'small functional sample, not a hardware performance qualification'}
                results.append(row)
                print(name, json.dumps(row['gpu_pair_ms']), flush=True)
    (args.out/'comparison.json').write_text(json.dumps(results, indent=2)+'\n')


if __name__ == '__main__':
    main()

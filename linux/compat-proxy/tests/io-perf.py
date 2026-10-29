#!/usr/bin/env python3
"""Paired host I/O benchmark; all generated files live outside the checkout.
Usage: io-perf.py [--before BASELINE_PROXY] [--loops 30000] [--runs 5]
Reports median microseconds and ratios, including native-negative lookups.
"""
import argparse
import json
import pathlib
import statistics
import subprocess
import tempfile

base = pathlib.Path(__file__).resolve().parents[1]
a = argparse.ArgumentParser(description=__doc__)
a.add_argument('--before', type=pathlib.Path)
a.add_argument('--proxy', type=pathlib.Path, default=base/'build/linux-layer-v3')
a.add_argument('--loops', type=int, default=30000)
a.add_argument('--runs', type=int, default=5)
args = a.parse_args()
if args.loops <= 0 or args.runs <= 0:
    a.error('loops and runs must be positive')
with tempfile.TemporaryDirectory(prefix='io-perf-', dir='/mnt/data/aosp/out/pc-logs') as tmp:
    root = pathlib.Path(tmp)
    tree = root/'tree'
    tree.mkdir()
    for i in range(128):
        d = tree/f'd{i:02d}'
        d.mkdir()
        (d/'file').write_bytes(b'x' * (args.loops * 2 + 4096))
    (tree/'link').symlink_to('d00/file')
    bench = root/'bench'
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(base/'tests/io-perf.c'), '-o', str(bench)], check=True)
    variants = {'native': [str(bench), str(tree), str(args.loops)]}
    for name, proxy in [('before', args.before), ('after', args.proxy)]:
        if proxy:
            variants[name] = [str(proxy.resolve()), '--android-filter', '--map', str(tree), '/io-tree', str(bench), '/io-tree', str(args.loops)]
    samples = {name: {} for name in variants}
    for _ in range(args.runs):
        for name, cmd in variants.items():
            p = subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=120)
            for line in p.stdout.splitlines():
                op, value, unit = line.split()
                assert unit == 'us/op'
                samples[name].setdefault(op, []).append(float(value))
    result = {name: {op: statistics.median(values) for op, values in ops.items()} for name, ops in samples.items()}
    result['ratios'] = {op: round(value / result['native'][op], 2) for op, value in result['after'].items()}
    print(json.dumps(result, indent=2))

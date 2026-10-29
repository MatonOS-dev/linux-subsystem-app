#!/usr/bin/env python3
"""Summarize trap-cost native/proxy and optional sigsys-cost/count captures.
Usage: trap-report.py NATIVE PROXY [--signal SIGNAL] [--counts COUNTS]
Never estimates device values from host results.
"""
import argparse
import collections
import json
import pathlib
import re
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('native', type=pathlib.Path)
parser.add_argument('proxy', type=pathlib.Path)
parser.add_argument('--signal', type=pathlib.Path)
parser.add_argument('--counts', type=pathlib.Path)
args = parser.parse_args()


def samples(path):
    values = collections.defaultdict(list)
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] == 'us/op':
            values[fields[0]].append(float(fields[1]))
    if not values:
        raise ValueError(f'no measurements in {path}')
    return {op: statistics.median(times) for op, times in values.items()}


native, proxy = samples(args.native), samples(args.proxy)
result = {'native_us': native, 'proxy_us': proxy,
          'ratios': {op: round(us/native[op], 3) for op, us in proxy.items() if op in native}}
if args.signal:
    signal = samples(args.signal)
    result['kernel_signal_us'] = signal
    result['kernel_signal_increment_us'] = signal['kernel-sigsys-return'] - signal['native-getppid']
    result['proxy_emulated_ioctl_minus_kernel_signal_us'] = proxy['ioctl-emulated'] - signal['kernel-sigsys-return']
if args.counts:
    header = pathlib.Path(__file__).resolve().parents[1]/'arch/syscalls.h'
    # The enum is contiguous after unknown=-1; these are operation IDs, not
    # x86 syscall numbers. Count captures can contain multiple process dumps.
    names = re.findall(r'^\s*PROXY_SC_(\w+)', header.read_text(), re.M)
    names.remove('unknown')
    totals = collections.defaultdict(lambda: [0, 0])
    for line in args.counts.read_text().splitlines():
        f = line.split()
        if len(f) == 5 and f[0] == 'OP':
            op = int(f[2])
            name = names[op] if 0 <= op < len(names) else str(op)
            totals[name][0] += int(f[3])
            totals[name][1] += int(f[4])
    result['counter_handler_us'] = {op: ns/n/1000 for op, (n, ns) in totals.items() if n}
    result['counter_calls'] = {op: n for op, (n, ns) in totals.items()}
    result['counter_note'] = ('OP excludes signal entry/return and handler tail; includes two raw clock calls per trap. '
                              'Use an uncounted launch for end-to-end latency. Subtractions are diagnostics, not additive phase timings.')
print(json.dumps(result, indent=2, sort_keys=True))

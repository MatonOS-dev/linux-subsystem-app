#!/usr/bin/env python3
"""Bound proxy RSS and anonymous mappings across a 4x hardlink copy."""
import pathlib
import re
import select
import subprocess
import sys
import tempfile
import time

base = pathlib.Path(__file__).resolve().parents[1]
loader = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else base / 'build/linux-layer-v3'
n = int(sys.argv[2]) if len(sys.argv) > 2 else 512

def sample(pid):
    status = pathlib.Path(f'/proc/{pid}/status').read_text()
    rss = int(re.search(r'^VmRSS:\s+(\d+)', status, re.M).group(1))
    maps = pathlib.Path(f'/proc/{pid}/maps').read_text().splitlines()
    anonymous = sum(len(line.split()) == 5 and line.split()[1] == 'rw-p' for line in maps)
    return rss, anonymous

with tempfile.TemporaryDirectory(prefix='steam-link-pressure-', dir='/mnt/data/aosp/out/pc-logs') as tmp:
    root = pathlib.Path(tmp)
    (root / '.v3-store-root').touch()
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(base / 'tests/link-pressure.c'),
                    '-o', str(root / 'copy')], check=True)
    link_deny = base / 'build/link-deny'
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(base / 'tests/link-deny.c'),
                    '-o', str(link_deny)], check=True)
    process = subprocess.Popen([str(link_deny), str(loader), '--android-filter', str(root / 'copy'), str(root), str(n)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    samples = {}
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        ready, _, _ = select.select([process.stdout], [], [], 0.1)
        if ready:
            line = process.stdout.readline()
            if not line:
                break
            line = line.strip()
            if line.startswith('PHASE '):
                phase = int(line.split()[1])
                samples[phase] = sample(process.pid)
                print(f'PHASE {phase}: VmRSS={samples[phase][0]} kB anonymous_rw_maps={samples[phase][1]}')
        if process.poll() is not None:
            break
    stdout, stderr = process.communicate(timeout=10)
    if process.returncode:
        raise SystemExit(f'pressure copy exited {process.returncode}\n{stdout}{stderr}')
    assert 1 in samples and 4 in samples, f'missing phase samples: {samples}; stdout={stdout}'
    rss_delta = samples[4][0] - samples[1][0]
    maps_delta = samples[4][1] - samples[1][1]
    print(f'growth for {3*n} additional links: VmRSS={rss_delta} kB anonymous_rw_maps={maps_delta}')
    assert rss_delta <= 8192 and maps_delta <= 32, 'proxy memory grew with the hardlink copy'
    print(f'PASS bounded proxy memory across {n} then {4*n} hardlinks')

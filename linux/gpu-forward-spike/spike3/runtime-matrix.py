#!/usr/bin/env python3
"""Run full glibc spike3 suite for installed/cached runtime loader+libc pairs.
No Flatpak downloads or system changes. Extra CLI roots can name cache trees.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

spike = Path(__file__).resolve().parent.parent
repo = spike.parent.parent
roots = [Path.home()/'.local/share/flatpak/runtime',
         repo/'tests/cache', repo/'tests/.cache', repo/'.cache',
         spike/'build/runtime-cache', repo/'linux/compat-proxy/tests/cache']
roots.extend(Path(p).resolve() for p in sys.argv[1:])
seen = set()
runtimes = []
for root in roots:
    if not root.is_dir():
        continue
    if root == roots[0]:
        # Known deployment layout: do not walk driver/locale extensions or the
        # entire OSTree store on this machine's spinning disk.
        candidates = root.glob('*/x86_64/*/*/files/lib/x86_64-linux-gnu/libc.so.6')
    else:
        candidates = root.rglob('libc.so.6')
    for libc in candidates:
        lib = libc.parent.resolve()
        loader = lib/'ld-linux-x86-64.so.2'
        if not loader.is_file() or lib in seen:
            continue
        seen.add(lib)
        version = subprocess.run([str(loader), '--version'], text=True,
                                 capture_output=True, check=True).stdout.splitlines()[0]
        assert re.search(r'\d+\.\d+', version), version
        runtimes.append((lib, loader, version))
if not runtimes:
    print('SKIP: no cached x86_64 Flathub runtime libc/loader pairs found')
    sys.exit(0)
failed = []
for lib, loader, version in sorted(runtimes):
    print(f'RUN {version}\n  {lib}', flush=True)
    env = dict(os.environ, SPIKE3_LOADER=str(loader), SPIKE3_LIBRARY_PATH=str(lib),
               SPIKE3_SKIP_PORTABILITY='1')
    result = subprocess.run(['bash', 'spike3/test.sh'], cwd=spike, env=env)
    print(f'{"PASS" if result.returncode == 0 else "FAIL"} {version}: {lib}', flush=True)
    if result.returncode:
        failed.append(str(lib))
print(f'Runtime matrix: {len(runtimes)-len(failed)}/{len(runtimes)} passed', flush=True)
sys.exit(bool(failed))

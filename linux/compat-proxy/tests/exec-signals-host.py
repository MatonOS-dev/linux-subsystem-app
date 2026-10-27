#!/usr/bin/env python3
"""Resident exec dispositions and default crash report, using real signals."""
import pathlib
import subprocess
root = pathlib.Path(__file__).resolve().parents[1]
subprocess.run(['cc', '-O0', '-Wall', '-Wextra', '-Werror',
                str(root / 'tests/exec-signals.c'), '-o',
                str(root / 'build/exec-signals')], check=True)
p = subprocess.run([str(root / 'build/linux-layer-v3'), '--android-filter',
                    str(root / 'build/exec-signals')], capture_output=True)
(root / 'build/exec-signals.log').write_bytes(p.stderr)
assert p.returncode == -11, (p.returncode, p.stderr)
for field in (b'v3 fatal signal=0x000000000000000b', b' addr=', b' pc=',
              b'v3 fatal executable=', b'/build/exec-signals',
              b'v3 fatal map=', b'v3 fatal image=', b' elf-offset='):
    assert field in p.stderr, p.stderr
print('PASS resident exec signal reset, SIG_IGN preservation and crash diagnostics')

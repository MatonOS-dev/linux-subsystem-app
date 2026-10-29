#!/usr/bin/env python3
"""Negative translation cache must observe external create and symlink."""
import pathlib
import subprocess
import tempfile

base = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='io-cache-', dir='/mnt/data/aosp/out/pc-logs') as tmp:
    root = pathlib.Path(tmp)
    exe = root/'test'
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(base/'tests/io-cache.c'), '-o', str(exe)], check=True)
    tree = root/'tree'
    tree.mkdir()
    for mapped in (False, True):
        cmd = [str(base/'build/linux-layer-v3'), '--android-filter']
        if mapped:
            cmd += ['--map', str(tree), '/cache-tree']
        cmd += [str(exe), '/cache-tree' if mapped else str(tree)]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        assert p.returncode == 0, p.stderr
        print(p.stdout, end='')

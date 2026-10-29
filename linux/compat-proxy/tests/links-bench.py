#!/usr/bin/env python3
"""Pressure-vessel-style tree population; optional before/after loader paths."""
import pathlib
import subprocess
import sys
import tempfile

base = pathlib.Path(__file__).resolve().parents[1]
loaders = [pathlib.Path(p).resolve() for p in sys.argv[1:]]
if not loaders:
    loaders = [base/'build/linux-layer-v3']
with tempfile.TemporaryDirectory(prefix='steam-links-bench-', dir='/mnt/data/aosp/out/pc-logs') as tmp:
    root = pathlib.Path(tmp)
    binary = root/'bench'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(base/'tests/links-bench.c'),'-o',str(binary)],check=True)
    for i, loader in enumerate([None] + loaders):
        fixture = root/str(i)
        fixture.mkdir(mode=0o700)
        (fixture/'.v3-store-root').touch()
        deep = fixture/'a/b/c/d/e/f/g/h'
        deep.mkdir(parents=True)
        cmd = [str(binary),str(deep)]
        if loader:
            cmd = [str(base/'build/link-deny'),str(loader),'--android-filter'] + cmd
        result = subprocess.run(cmd,capture_output=True,text=True,timeout=120)
        assert result.returncode == 0,result.stderr
        print(str(loader) if loader else 'native')
        print(result.stdout,end='',flush=True)

#!/usr/bin/env python3
"""Opt-in diagnostics retain payload errors after stderr redirection."""
import pathlib,subprocess
root=pathlib.Path(__file__).resolve().parents[1]
child=root/'build/stderr-trace'
subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(root/'tests/stderr-trace.c'),'-o',str(child)],check=True)
for name in ('linux-layer-v3','linux-layer-v3-i386'):
    for trace in (False,True):
        args=[str(root/'build'/name),'--android-filter']
        if trace:args+=['--trace']
        p=subprocess.run(args+[str(child)],capture_output=True,timeout=15)
        assert p.returncode==0,(name,p.returncode,p.stderr[-2000:])
        for marker in (b'redirected-write',b'redirected-writev'):
            assert (marker in p.stderr)==trace,(name,trace,p.stderr[-2000:])
    print(f'PASS {name} trace retains redirected write/writev stderr only when enabled')

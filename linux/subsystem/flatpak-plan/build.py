#!/usr/bin/env python3
"""Build the public-API Flatpak inspection adapter against the pinned build."""
import os, pathlib, shlex, subprocess, sys

source = pathlib.Path(__file__).resolve().parent
out = pathlib.Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
build = pathlib.Path(os.environ['MATON_FLATPAK_PLAN_BUILD'])

def command(target):
    lines = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()
    return shlex.split(lines[-1])

# Reuse the pinned dependency link set but compile this source with public
# headers and no upstream private object or private header dependency.
compile = command('common/libflatpak-common.a.p/flatpak-installation.c.o')
for flag in ('-o', '-MF', '-MQ'):
    if flag in compile:
        compile[compile.index(flag) + 1] = str(out / ('inspect.o.d' if flag == '-MF' else 'inspect.o'))
compile = [str(source / 'inspect.c') if arg.endswith('/flatpak-installation.c') else arg for arg in compile]
subprocess.check_call(compile, cwd=build)
link = [arg for arg in command('common/test-libflatpak') if not arg.endswith('.o')]
link[link.index('-o') + 1] = str(out / 'libmatonos-flatpak-plan.so')
link.insert(1, str(out / 'inspect.o'))
subprocess.check_call(link, cwd=build)
subprocess.check_call([os.environ.get('STRIP', 'strip'), '--strip-all', str(out / 'libmatonos-flatpak-plan.so')])
headers = subprocess.check_output(['readelf', '-l', str(out / 'libmatonos-flatpak-plan.so')], text=True)
if 'INTERP' in headers:
    sys.exit('Flatpak plan helper unexpectedly has PT_INTERP')

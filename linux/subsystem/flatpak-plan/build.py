#!/usr/bin/env python3
"""Link our read-only adapter to the pinned, already built Flatpak libraries."""
import os, pathlib, shlex, subprocess, sys
source=pathlib.Path(__file__).resolve().parent
out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
aosp=pathlib.Path(os.environ['MATON_AOSP_ROOT'])
paths=dict(line.split(': ',1) for line in (aosp/'paths.md').read_text().splitlines() if ': ' in line)
build=pathlib.Path(os.environ.get('MATON_FLATPAK_PLAN_BUILD',str(pathlib.Path(paths['apexs'])/'flatpak/build/flatpak')))
def commands(target):return shlex.split(subprocess.check_output(['ninja','-C',str(build),'-t','commands',target],text=True).strip().splitlines()[-1])
compile=commands('common/libflatpak-common.a.p/flatpak-installation.c.o')
for flag in ['-o','-MF','-MQ']:
    if flag in compile:compile[compile.index(flag)+1]=str(out/('inspect.o'+('.d' if flag=='-MF' else '')))
compile=[str(source/'inspect.c') if arg.endswith('/flatpak-installation.c') else arg for arg in compile]
subprocess.check_call(compile,cwd=build)
# The library test's link recipe has the public/private Flatpak libraries and
# their pinned static dependencies. Drop its test object, retain the libraries.
link=[arg for arg in commands('common/test-libflatpak') if not arg.endswith('.o')]
link[link.index('-o')+1]=str(out/'libmatonos-flatpak-plan.so')
link.insert(1,str(out/'inspect.o'))
subprocess.check_call(link,cwd=build)
subprocess.check_call([str(pathlib.Path(link[0]).parent/'llvm-strip'),'--strip-all',str(out/'libmatonos-flatpak-plan.so')])

#!/usr/bin/env python3
"""Package the pinned Xwayland build as APK native libraries and data."""
import json, os, shutil, subprocess, sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
build=Path(os.environ['MATON_COMPOSITOR_BUILD'])
x=build/'xstage/x86_64'
config=(build/'xwayland-stack/xwayland/include/xkb-config.h').read_text()
if '#define XKB_BIN_DIRECTORY "."' not in config:
    raise RuntimeError('Rebuild Xwayland with -Dxkb_bin_dir=. before packaging')
out=Path(sys.argv[1]); jni=out/'jni/x86_64'; assets=out/'assets/xwayland'
shutil.rmtree(out,ignore_errors=True);jni.mkdir(parents=True);assets.mkdir(parents=True)
links={}
def copy(source,name,link=None):
    if not source.is_file():raise RuntimeError(f'Missing Xwayland dependency: {source}')
    shutil.copyfile(source,jni/name)
    if link:links[link]=name
copy(x/'bin/Xwayland','libmatonos-Xwayland-server.so')
copy(x/'bin/xkbcomp','libmatonos-xkbcomp.so')
existing=build/'jniLibs/x86_64'
for source in sorted((x/'lib').iterdir()):
    if '.so' not in source.name or source.name.startswith(('libpthread','librt')):continue
    if (existing/source.name).is_file():continue
    name=source.name if source.name.endswith('.so') else 'libmatonos-xwayland-'+source.name+'.so'
    copy(source,name,source.name)
for name in ['libEGL.so','libGLESv2.so','libgbm.so']:
    copy(x/'lib/xwayland-egl'/name,'libmatonos-xwayland-'+name[3:],name)
# The server uses Bionic crypto; these are distinct from the static musl CLI.
for name in ['libgcrypt.so','libgpg-error.so']:copy(x/'lib'/name,name,name)
shutil.copytree(build/'xkb-data/system_ext/share/xkeyboard-config-2',assets/'xkb',symlinks=False)
# Preserve upstream copyright/licence notices with the runtime.
for source in (build/'sources/src').iterdir():
    if not source.is_dir():continue
    for pattern in ['COPYING*','LICENSE*','docs/FTL.TXT']:
        for notice in source.glob(pattern):
            if notice.is_file():
                target=assets/'licenses'/source.name/notice.name
                target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(notice,target)
crypto_sources=build/'sources/src'
for name in ['libgcrypt-1.11.1','libgpg-error-1.51']:
    source=crypto_sources/name
    for notice in ['COPYING','COPYING.LIB','AUTHORS']:
        if (source/notice).is_file():
            target=assets/'licenses'/name/notice;target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(source/notice,target)
(assets/'libraries.json').write_text(json.dumps(links,sort_keys=True))
shutil.copyfile(root/'linux/subsystem/UPSTREAMS',assets/'SOURCE')
ndk=Path(os.environ['MATON_NDK'])/'toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang'
subprocess.run([str(ndk),'-std=c11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-fPIE','-pie',str(root/'linux/subsystem/native/xwayland-launch.c'),'-o',str(jni/'libmatonos-Xwayland.so')],check=True)

#!/usr/bin/env python3
"""Fail if any native library in the APK needs a library the device won't provide.

A DT_NEEDED entry is satisfied by another lib/x86_64 entry of the APK, by a
private SONAME link the Xwayland launcher creates (assets/xwayland/libraries.json),
or by a library in the NDK's public system library set for the APK's ABI.
"""
import json, pathlib, subprocess, sys, tempfile, zipfile

apk, ndk = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
readelf = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf'
sysroot = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android'
system = set()
for api in sysroot.iterdir():
    if api.is_dir():
        system.update(p.name for p in api.glob('*.so'))
with zipfile.ZipFile(apk) as z:
    libs = {pathlib.PurePosixPath(n).name: n for n in z.namelist() if n.startswith('lib/x86_64/') and n.endswith('.so')}
    links = {}
    for n in z.namelist():
        if n.endswith('assets/xwayland/libraries.json') or n == 'assets/xwayland/libraries.json':
            links = json.loads(z.read(n))
    available = set(libs) | set(links) | system
    missing = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, entry in sorted(libs.items()):
            path = pathlib.Path(tmp) / name
            path.write_bytes(z.read(entry))
            out = subprocess.run([str(readelf), '-d', str(path)], capture_output=True, text=True, check=True).stdout
            for line in out.splitlines():
                if '(NEEDED)' in line:
                    needed = line.split('[', 1)[1].split(']', 1)[0]
                    if needed not in available:
                        missing.append(f'{name} needs {needed}')
if missing:
    print('APK native dependency closure is incomplete:', file=sys.stderr)
    for m in missing:
        print('  ' + m, file=sys.stderr)
    sys.exit(1)
print(f'PASS APK native dependency closure: {len(libs)} libraries')

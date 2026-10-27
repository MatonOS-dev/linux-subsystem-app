#!/usr/bin/env bash
# Host-only: copies build artifacts, never accesses adb or Android installs.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD/build/musl-host-root
./build.sh
python3 - "$root" <<'PY'
import os, shutil, sys
root, = sys.argv[1:]
tool_dir=root+'/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64'
os.makedirs(tool_dir,exist_ok=True)
for source,target in (('matonos-flatpak','libmatonos-flatpak.so'),('matonos-app-flatpak','libmatonos-app-flatpak.so'),('matonos-app-bwrap','libmatonos-app-bwrap.so')):
    shutil.copy2('../flatpak/prebuilt/static/x86_64/'+source,tool_dir+'/'+target)
# The helper sets argv[0] to these applet names before exec; aliases model
# that dispatch for this host-side fixture without weakening its UID check.
for app in ('flatpak','bwrap'):
    link=tool_dir+'/'+app
    if os.path.lexists(link): os.unlink(link)
os.symlink('libmatonos-flatpak.so',tool_dir+'/flatpak')
os.symlink('libmatonos-flatpak.so',tool_dir+'/bwrap')
PY
for app in bwrap flatpak; do
    timeout 20 build/linux-layer-v3 --android-filter --map "$root/system_ext" /system_ext "/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/$app" --version >"build/musl-$app.out" 2>"build/musl-$app.log"
    cat "build/musl-$app.out"
done
mkdir -p build/musl-fixture
printf 'mount-ok\n' >build/musl-fixture/fixture
proxy=(build/linux-layer-v3 --android-filter --map "$root/system_ext" /system_ext)
bwrap=/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/bwrap
timeout 20 "${proxy[@]}" "$bwrap" --ro-bind / / --tmpfs /tmp --proc /proc --dev /dev /bin/true >build/musl-root.out 2>build/musl-root.log
timeout 20 "${proxy[@]}" "$bwrap" --ro-bind / / --tmpfs /tmp --bind "$PWD/build/musl-fixture" /tmp/fixture --proc /proc --dev /dev /bin/cat /tmp/fixture/fixture >build/musl-fixture.out 2>build/musl-fixture.log
grep -qx mount-ok build/musl-fixture.out
printf 'PASS staged static musl bwrap -> host root and fixture payloads\n'

mkdir -p build/flatpak-bwrap/app
cc -O2 -Wall -Wextra -Werror tests/filter-check.c -o build/flatpak-bwrap/app/filter-check
python3 tests/flatpak-bwrap-host.py

# Portal Spawn: inherited app filter/zero quota -> Flatpak argv boundary ->
# actual shipped bwrap --args fd -> fork + devpts remapping + payload exec.
cc -no-pie -O2 -Wall -Wextra -Werror tests/portal-userns.c -o build/portal-userns
timeout 20 build/linux-layer-v3 --android-filter --trace "$PWD/build/portal-userns" launch "$root/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/libmatonos-app-bwrap.so" >build/portal-userns.out 2>build/portal-userns.log
cat build/portal-userns.out
grep -q 'bwrap-devpts-userns' build/portal-userns.log

python3 tests/proc-dirfd-host.py

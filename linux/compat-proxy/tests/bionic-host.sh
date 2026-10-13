#!/usr/bin/env bash
# Host-only: copies build artifacts, never accesses adb or Android installs.
set -euo pipefail
cd "$(dirname "$0")/.."
product=${COMPAT_PRODUCT_OUT:-/mnt/data/aosp/out/target/product/pc_x86_64}
root=$PWD/build/bionic-host-root
./build.sh
python3 - "$product" "$root" <<'PY'
import os, shutil, sys
product, root = sys.argv[1:]
for name in ('apex/com.android.runtime', 'apex/com.matonos.flatpak'):
    shutil.copytree(product+'/'+name, root+'/'+name, symlinks=True, dirs_exist_ok=True)
for name in ('system/bin','system/lib64'):
    os.makedirs(root+'/'+name,exist_ok=True)
for name in ('libc.so','libm.so','libdl.so'):
    path=root+'/system/lib64/'+name
    if not os.path.lexists(path):
        os.symlink('/apex/com.android.runtime/lib64/bionic/'+name,path)
path=root+'/system/bin/linker64'
if not os.path.lexists(path):os.symlink('/apex/com.android.runtime/bin/linker64',path)
for name in ('proc','dev'):
    path=root+'/'+name
    if not os.path.lexists(path):os.symlink('/'+name,path)
PY
for app in bwrap flatpak; do
    timeout 20 build/linux-layer-v3 --android-filter --root "$root" "/apex/com.matonos.flatpak/bin/matonos-$app" --version >"build/bionic-$app.out" 2>"build/bionic-$app.log"
    cat "build/bionic-$app.out"
done
mkdir -p build/bionic-fixture
printf 'mount-ok\n' >build/bionic-fixture/fixture
proxy=(build/linux-layer-v3 --android-filter --map "$root/system" /system --map "$root/apex" /apex)
bwrap=/apex/com.matonos.flatpak/bin/matonos-app-bwrap
timeout 20 "${proxy[@]}" "$bwrap" --ro-bind / / --tmpfs /tmp --proc /proc --dev /dev /bin/true >build/bionic-root.out 2>build/bionic-root.log
timeout 20 "${proxy[@]}" "$bwrap" --ro-bind / / --tmpfs /tmp --bind "$PWD/build/bionic-fixture" /tmp/fixture --proc /proc --dev /dev /bin/cat /tmp/fixture/fixture >build/bionic-fixture.out 2>build/bionic-fixture.log
grep -qx mount-ok build/bionic-fixture.out
printf 'PASS staged bionic bwrap -> host glibc root and fixture payloads\n'

mkdir -p build/flatpak-bwrap/app
cc -O2 -Wall -Wextra -Werror tests/filter-check.c -o build/flatpak-bwrap/app/filter-check
python3 tests/flatpak-bwrap-host.py

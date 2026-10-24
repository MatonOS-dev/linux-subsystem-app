#!/usr/bin/env bash
# Device-only bionic/glibc adoption test, independent of installed GPU drivers.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?usage: i386-adopt-device.sh external-output-dir [adb-serial]}
mkdir -p "$output"
output=$(cd "$output" && pwd)
case "$output/" in "$source_dir/"*) echo 'Use an output directory outside the source tree' >&2;exit 2;; esac
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang
adb_bin=${ADB:-/mnt/data/aosp/out/host/linux-x86/bin/adb}
adb=("$adb_bin" -s "${2:-127.0.0.1:5556}")
libc=${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}
interpreter=${MATON_V3_I386_INTERPRETER:-/lib/ld-linux.so.2}
# Use the checked-out AOSP app policy even on a clean source checkout.
python3 "$source_dir/gen-policy.py"
device_dir=/data/local/tmp/maton-i386-adopt-$$
trap '"${adb[@]}" shell "rm -rf $device_dir" >/dev/null' EXIT
"$compiler" -DPROXY_ARCH_BACKEND='"i386/backend.h"' -DMATON_GPU_BRIDGE -DPROXY_LIBRARY \
    -fPIE -pie -Wl,-z,now -Wl,--image-base=0x70000000 \
    -Wl,--section-start=.proxy_gateway=0x70300000 \
    -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
    "$source_dir"/{loader.c,proxy.c,vfs.c,links.c,net.c,sysvipc.c,state.c} \
    "$source_dir"/arch/i386/{raw.S,adopt.S,thread-adopt.c} \
    "$source_dir/tests/i386-adopt-android.c" -o "$output/adopt-android"
"$compiler" -c -O2 -fno-stack-protector -Wall -Wextra -Werror \
    "$source_dir/tests/i386-adopt-payload.c" -o "$output/payload.o"
cc -m32 -c "$source_dir/tests/i386-entry.S" -o "$output/entry.o"
ld -m elf_i386 --dynamic-linker "$device_dir/ld-linux.so.2" \
    -o "$output/adopt-payload" "$output/entry.o" "$output/payload.o" "$libc"
"${adb[@]}" shell "mkdir -p $device_dir"
"${adb[@]}" push "$output/adopt-android" "$output/adopt-payload" "$libc" "$device_dir/"
"${adb[@]}" push "$interpreter" "$device_dir/ld-linux.so.2"
"${adb[@]}" shell "chmod 755 $device_dir/*; TMPDIR=$device_dir LD_LIBRARY_PATH=$device_dir timeout 15 $device_dir/adopt-android $device_dir/adopt-payload" | tee "$output/result.log"
grep -q 'PASS i386 bionic adoption:' "$output/result.log"

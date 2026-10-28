#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?external output directory}
aosp=${MATON_AOSP_ROOT:-/mnt/data/aosp}
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang
runtime=$aosp/out/target/product/pc_x86_64/apex/com.android.runtime
mkdir -p "$output"
output=$(realpath "$output")
cc -O2 -Wall -Wextra -Werror "$root/tests/bionic32-start.c" -o "$output/start"
"$compiler" -shared -fPIC -O2 -Wall -Wextra -Werror "$root/tests/bionic32-peer.c" -Wl,-soname,libbionic32-peer.so -o "$output/libbionic32-peer.so"
"$compiler" -nostdlib -fPIC -pie -Wl,--no-dynamic-linker \
 -Wl,--image-base=0x70000000 -Wl,--section-start=.proxy_gateway=0x70300000 \
 -Wl,--no-as-needed -Wl,-rpath,"$output" -L"$output" -l:libbionic32-peer.so \
 -DPEER_TEST_LINKER=\"$runtime/bin/linker\" -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
 "$root/arch/i386/peer-start.c" "$root/arch/i386/peer-start.S" -o "$output/peer"
python3 - "$output/peer" <<'PYCODE'
import sys
with open(sys.argv[1],'r+b') as f:
    f.seek(16)
    assert f.read(2)==b'\x03\x00'
    f.seek(16)
    f.write(b'\x02\x00')
PYCODE
# Android's ELF32 pthread owner-TID is 16 bits. A host with a large pid_max
# needs an isolated PID namespace, otherwise native bionic mutexes deadlock.
for base in 0x10001000 0x37a53000 0x4ffff000; do
 unshare -Urpf --mount-proc env LD_LIBRARY_PATH="$runtime/lib/bionic" \
  timeout 15 "$output/start" "$output/peer" "$base"
done

#!/usr/bin/env bash
# Production ELF32 peer with an inherited ioctl denial; no VM or GPU service.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external output directory}
aosp=${MATON_AOSP_ROOT:-/mnt/data/aosp}
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang
runtime=$aosp/out/target/product/pc_x86_64/apex/com.android.runtime
mkdir -p "$out"
out=$(realpath "$out")
MATON_V3_ANDROID_OUT="$out/android" bash "$root/build-android.sh"
cc -O2 -Wall -Wextra -Werror "$root/tests/bionic32-terminal-start.c" -o "$out/start"
"$compiler" -shared -fPIC -O2 -Wall -Wextra -Werror \
 "$root/tests/bionic32-terminal-provider.c" -ldl -o "$out/provider.so"
"$compiler" -c -O2 -Wall -Wextra -Werror \
 "$root/tests/i386-terminal.c" -o "$out/payload.o"
cc -m32 -c "$root/tests/i386-entry.S" -o "$out/entry.o"
cc -m32 -c "$root/tests/i386-terminal.S" -o "$out/terminal.o"
ld -m elf_i386 -o "$out/payload" --dynamic-linker /lib/ld-linux.so.2 \
 "$out/entry.o" "$out/terminal.o" "$out/payload.o" "${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}"
"$compiler" -nostdlib -fPIC -pie -Wl,--no-dynamic-linker \
 -Wl,--image-base=0x70000000 -Wl,--section-start=.proxy_gateway=0x70300000 \
 -Wl,--no-as-needed -Wl,-rpath,"$out/android" -L"$out/android" -l:libmatonos-v3-peer-i386.so \
 -DPEER_TEST_LINKER=\"$runtime/bin/linker\" -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
 "$root/arch/i386/peer-start.c" "$root/arch/i386/peer-start.S" -o "$out/peer"
python3 - "$out/peer" <<'PY'
import sys
with open(sys.argv[1],'r+b') as f:
    f.seek(16)
    assert f.read(2)==b'\x03\x00'
    f.seek(16)
    f.write(b'\x02\x00')
PY
for rewrite in 0 1; do
unshare -Urpf --mount-proc env MATON_PROXY_REWRITE=$rewrite \
 LD_LIBRARY_PATH="$aosp/out/target/product/pc_x86_64/system/lib:$runtime/lib/bionic" \
 MATON_TEST_GPU_PROVIDER="$out/provider.so" \
 timeout 30 "$out/start" "$out/peer" 0x30000000 peer --gpu-bridge \
 --android-filter --map /tmp /terminal-fixture "$out/payload"
done

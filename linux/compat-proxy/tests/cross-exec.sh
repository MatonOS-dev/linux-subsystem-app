#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang
libc=${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}
interpreter=${MATON_V3_I386_INTERPRETER:-/lib/ld-linux.so.2}
cc -no-pie -O2 -Wall -Wextra -Werror tests/cross-exec.c -o build/cross64
"$compiler" -c -O2 -fno-stack-protector -Wall -Wextra -Werror tests/cross-exec.c -o build/cross32.o
ld -m elf_i386 -o build/cross32 --dynamic-linker "$interpreter" build/i386-p1/entry.o build/cross32.o "$libc"
printf 'cross\n' >build/cross-file
mkdir -p build/cross-dir
chmod 0700 build/cross-dir
for arch in 64 32; do
    if [[ $arch == 64 ]]; then loader=linux-layer-v3; peer=32; else loader=linux-layer-v3-i386; peer=64; fi
    timeout 20 "build/$loader" --android-filter --trace --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
        "$PWD/build/cross$arch" "$PWD/build/cross$peer" "$PWD/build/cross$arch" \
        >"build/cross$arch.out" 2>"build/cross$arch.log"
    cat "build/cross$arch.out"
done

if command -v bwrap >/dev/null; then
    timeout 20 build/linux-layer-v3 --android-filter --map "$PWD/build/cross-file" /v3-cross-file --map "$PWD/build/cross-dir" /v3-cross-dir \
        "$(command -v bwrap)" --ro-bind / / --proc /proc --dev /dev \
        "$PWD/build/cross32" "$PWD/build/cross64" "$PWD/build/cross32" >build/cross-bwrap.out 2>build/cross-bwrap.log
    cat build/cross-bwrap.out
fi

python3 tests/state-reject.py

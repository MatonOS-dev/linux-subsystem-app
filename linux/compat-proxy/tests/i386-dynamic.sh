#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang
libc=${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}
interpreter=${MATON_V3_I386_INTERPRETER:-/lib/ld-linux.so.2}
if [[ ! -f "$libc" || ! -f "$interpreter" ]]; then
    echo 'EXTERNAL P2 blocker: provide an i386 glibc and its interpreter.' >&2
    exit 1
fi
mkdir -p build/i386-p1
cc -m32 -c tests/i386-entry.S -o build/i386-p1/entry.o
for test in dynamic sockets dns ipc pty fp resolver nofollow; do
    "$compiler" -c -O2 -fno-stack-protector -Wall -Wextra -Werror \
        "tests/i386-$test.c" -o "build/i386-p1/$test.o"
    ld -m elf_i386 -o "build/i386-p1/$test" --dynamic-linker "$interpreter" \
        build/i386-p1/entry.o "build/i386-p1/$test.o" "$libc"
done
for test in dynamic sockets ipc pty fp; do
    timeout 15 "build/i386-p1/$test" >"build/i386-p1/$test-native.out"
    timeout 15 build/linux-layer-v3-i386 --android-filter "$PWD/build/i386-p1/$test" \
        >"build/i386-p1/$test.out" 2>"build/i386-p1/$test.log"
    cmp "build/i386-p1/$test-native.out" "build/i386-p1/$test.out"
    cat "build/i386-p1/$test.out"
done
python3 tests/i386-dns.py

python3 tests/i386-resolver.py

timeout 15 build/linux-layer-v3-i386 --android-filter "$PWD/build/i386-p1/nofollow"

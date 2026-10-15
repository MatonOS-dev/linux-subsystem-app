#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang
mkdir -p build/exec-errors
b=$PWD/build/exec-errors
mkdir -p build/i386-p1
cc -m32 -c tests/i386-entry.S -o build/i386-p1/entry.o
cc -no-pie -O2 -Wall -Wextra -Werror tests/exec-errors.c -o "$b/caller64"
"$compiler" -c -O2 -fno-stack-protector -Wall -Wextra -Werror tests/exec-errors.c -o "$b/caller32.o"
ld -m elf_i386 -o "$b/caller32" --dynamic-linker /lib/ld-linux.so.2 build/i386-p1/entry.o "$b/caller32.o" /usr/lib32/libc.so.6
# Same fixed addresses as the caller: a failed exec must not overwrite it.
cc -no-pie tests/exec-errors.c -Wl,--dynamic-linker=/nonexistent/v3-ld.so -o "$b/bad64"
ld -m elf_i386 -o "$b/bad32" --dynamic-linker /nonexistent/v3-ld.so build/i386-p1/entry.o "$b/caller32.o" /usr/lib32/libc.so.6
for target in 64 32; do
    cp "$b/bad$target" "$b/noexec$target"; chmod 600 "$b/noexec$target"
    head -c 128 "$b/bad$target" > "$b/truncated$target"; chmod 700 "$b/truncated$target"
done
longpath=$(printf 'x%.0s' {1..4097})
for caller in 64 32; do
    if [[ $caller == 64 ]]; then loader=build/linux-layer-v3; else loader=build/linux-layer-v3-i386; fi
    for target in 64 32; do
        timeout 20 "$loader" --android-filter "$b/caller$caller" "$b/bad$target" "$b/missing$target" "$b/noexec$target" "$b/truncated$target" "$longpath" >"$b/$caller-$target.out" 2>"$b/$caller-$target.log"
        cat "$b/$caller-$target.out"
    done
done

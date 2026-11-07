#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external output directory}
mkdir -p "$out"
cc=${CC:-clang-21}
"$cc" -O2 -Wall -Wextra -Werror -fno-builtin -I"$here" -I"$here/arch/x86_64" \
 "$here/tests/syscall-gateway.c" "$here/tests/syscall-gateway.S" "$here/runtime.c" -o "$out/gateway64"
"$out/gateway64"
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang
for unit in tests/syscall-gateway.c tests/syscall-gateway.S runtime.c; do
 "$compiler" -c -O2 -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
  -I"$here" -I"$here/arch/i386" "$here/$unit" -o "$out/$(basename "$unit").o"
done
"$compiler" -c "$here/../compat-proxy/arch/i386/entry.S" -o "$out/entry.o"
ld -m elf_i386 --dynamic-linker /lib/ld-linux.so.2 "$out/entry.o" \
 "$out/syscall-gateway.c.o" "$out/syscall-gateway.S.o" "$out/runtime.c.o" \
 "${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}" -o "$out/gateway32"
"$out/gateway32"

#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external output directory}
mkdir -p "$out"
cc -DPROXY_LIBRARY -nostdlib -static -no-pie -mcmodel=large \
 -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 \
 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
 "$root/tests/native-reservation.c" "$root"/{loader.c,vfs.c,links.c,net.c,sysvipc.c,state.c} \
 "$root"/arch/x86_64/{raw.S,entry.S} -o "$out/native-reservation"
"$out/native-reservation"
cc -DPROXY_LIBRARY -nostdlib -static -no-pie -mcmodel=large \
 -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 \
 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
 "$root/tests/native-state.c" "$root"/{loader.c,vfs.c,links.c,net.c,sysvipc.c,state.c} \
 "$root"/arch/x86_64/{raw.S,entry.S} -o "$out/native-state"
cc -O2 -Wall -Wextra -Werror "$root/tests/native-state-payload.c" -o "$out/payload64"
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang" \
 -c -O2 -Wall -Wextra -Werror "$root/tests/native-state-payload.c" -o "$out/payload32.o"
cc -m32 -c "$root/tests/i386-entry.S" -o "$out/entry.o"
ld -m elf_i386 --dynamic-linker /lib/ld-linux.so.2 -o "$out/payload32" \
 "$out/entry.o" "$out/payload32.o" "${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}"
# The real state handoff validates sibling loader ownership and ELF class.
cp "$root/build/linux-layer-v3" "$out/libmatonos-v3-loader-static.so"
cp "$root/build/linux-layer-v3-i386" "$out/libmatonos-v3-loader-i386.so"
chmod 0755 "$out" "$out"/libmatonos-v3-loader*.so
timeout 20 "$out/native-state" "$out/payload64" "$out/payload32" "$out/payload64"

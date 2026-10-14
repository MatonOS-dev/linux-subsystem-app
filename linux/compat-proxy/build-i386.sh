#!/usr/bin/env bash
# Freestanding ELF32; the NDK supplies headers even without host multilib dev.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
output=${1:-$source_dir/build/linux-layer-v3-i386}
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=${MATON_V3_I386_CC:-$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang}
mkdir -p "$(dirname "$output")"
python3 "$source_dir/gen-policy.py"
builtins=$("$compiler" --print-libgcc-file-name)
"$compiler" -m32 -DPROXY_ARCH_BACKEND='"i386/backend.h"' \
  -nostdlib -static -Wl,-no-pie -Wl,--image-base=0x70000000 \
  -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
  "$source_dir"/{loader.c,proxy.c,vfs.c,links.c,net.c,sysvipc.c} \
  "$source_dir"/arch/i386/{raw.S,entry.S} "$builtins" -o "$output"

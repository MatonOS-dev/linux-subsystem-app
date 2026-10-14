#!/usr/bin/env bash
# Freestanding fixed-address x86_64 ELF: no bionic/linker dependency.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
aosp_root=${MATON_AOSP_ROOT:-/mnt/data/aosp}
output=${MATON_V3_ANDROID_OUT:-$aosp_root/out/matonos/compat-proxy}
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang
mkdir -p "$output"
python3 "$source_dir/gen-policy.py"
"$compiler" -nostdlib -static -Wl,-no-pie -mcmodel=large -Wl,--image-base=0x600000000000 \
  -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
  "$source_dir"/{loader.c,proxy.c,vfs.c,links.c,net.c,sysvipc.c,raw.S,entry.S} \
  -o "$output/libmatonos-v3-loader.so"

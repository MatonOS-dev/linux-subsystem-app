#!/usr/bin/env bash
# Freestanding fixed-address x86_64 ELF: no bionic/linker dependency.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
aosp_root=${MATON_AOSP_ROOT:-/mnt/data/aosp}
output=${MATON_V3_ANDROID_OUT:-$aosp_root/out/matonos/compat-proxy}
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang
mkdir -p "$output"
chmod 0755 "$output"
# Generated policy and all compiler outputs remain in the external build root.
build_source=$output/freestanding/compat-proxy
mkdir -p "$build_source" "$output/freestanding/packs"
cp -a "$source_dir"/*.c "$source_dir"/*.h "$source_dir/arch" "$source_dir/gen-policy.py" "$source_dir/build-i386.sh" "$build_source/"
cp -a "$source_dir/../packs/." "$output/freestanding/packs/"
python3 "$build_source/gen-policy.py"
"$compiler" -nostdlib -static -Wl,-no-pie -mcmodel=large -Wl,--image-base=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 \
  -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
  "$build_source"/{loader.c,proxy.c,vfs.c,links.c,net.c,sysvipc.c,state.c} \
  "$build_source"/arch/x86_64/{raw.S,entry.S} \
  -o "$output/libmatonos-v3-loader.so"

bash "$build_source/build-i386.sh" "$output/libmatonos-v3-loader-i386.so"

chmod 0755 "$output/libmatonos-v3-loader.so"


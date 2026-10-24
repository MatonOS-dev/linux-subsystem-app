#!/usr/bin/env bash
# Freestanding ELF32; the NDK supplies headers even without host multilib dev.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
source "$(dirname "$source_dir")/i386-flags.sh"
output=${1:-$source_dir/build/linux-layer-v3-i386}
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=${MATON_V3_I386_CC:-$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android36-clang}
counter_flags=()
if [[ ${PROXY_RAW_COUNTER:-0} == 1 ]]; then counter_flags=(-DPROXY_RAW_COUNTER); fi
mkdir -p "$(dirname "$output")"
chmod 0755 "$(dirname "$output")"
python3 "$source_dir/gen-policy.py"
builtins=$("$compiler" --print-libgcc-file-name)
link_flags=(-nostdlib -static -Wl,-no-pie)
backend_sources=(
  "$source_dir/arch/i386/entry.S"
  "$source_dir/arch/i386/sysinfo.S"
  "$source_dir/arch/i386/sysinfo.c"
)
if [[ ${MATON_I386_GPU_BRIDGE:-0} == 1 ]]; then
  link_flags=(-DMATON_GPU_BRIDGE -fPIE -pie -Wl,-z,now)
  backend_sources=(
    "$source_dir/arch/i386/sysinfo.S"
    "$source_dir/arch/i386/sysinfo.c"
    "$source_dir/arch/i386/adopt.S"
    "$source_dir/arch/i386/thread-adopt.c"
    -pthread -ldl -lEGL -lGLESv2
  )
fi
"$compiler" "${MATON_I386_ISA_FLAGS[@]}" "${counter_flags[@]}" -DPROXY_ARCH_BACKEND='"i386/backend.h"' \
  "${link_flags[@]}" -Wl,--image-base=0x70000000 -Wl,--section-start=.proxy_gateway=0x70300000 \
  -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
  "$source_dir"/{loader.c,proxy.c,vfs.c,links.c,net.c,sysvipc.c,state.c} \
  "$source_dir"/arch/i386/raw.S "${backend_sources[@]}" "$builtins" -o "$output"

chmod 0755 "$output"

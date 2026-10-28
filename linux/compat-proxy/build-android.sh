#!/usr/bin/env bash
# Production x86_64 loader: bionic stays resident for optional GPU thread adoption.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
aosp_root=${MATON_AOSP_ROOT:-/mnt/data/aosp}
output=${MATON_V3_ANDROID_OUT:-$aosp_root/out/matonos/compat-proxy}
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang
counter_flags=()
if [[ ${PROXY_RAW_COUNTER:-0} == 1 ]]; then counter_flags=(-DPROXY_RAW_COUNTER); fi
mkdir -p "$output"
chmod 0755 "$output"
# Generated policy and all compiler outputs remain in the external build root.
build_source=$output/freestanding/compat-proxy
mkdir -p "$build_source" "$output/freestanding/packs"
cp -a "$source_dir"/*.c "$source_dir"/*.h "$source_dir/arch" "$source_dir/gen-policy.py" "$source_dir/build-i386.sh" "$build_source/"
cp -a "$source_dir/../packs/." "$output/freestanding/packs/"
mkdir -p "$output/freestanding/gpu-forward-spike/spike3"
cp "$source_dir/../gpu-forward-spike/"{bootstrap.h,bridge.h} "$output/freestanding/gpu-forward-spike/"
cp "$source_dir/../gpu-forward-spike/spike3/adopt.h" "$output/freestanding/gpu-forward-spike/spike3/"
mkdir -p "$output/freestanding/gpu-bridge-common"
cp "$source_dir/../gpu-bridge-common/"{bootstrap.h,bridge.h,adopt.h} "$output/freestanding/gpu-bridge-common/"
python3 "$build_source/gen-policy.py"
"$compiler" "${counter_flags[@]}" -march=x86-64-v2 -DMATON_GPU_BRIDGE -fPIE -pie -mcmodel=large -Wl,--image-base=0x600000000000 -Wl,-z,now -Wl,--export-dynamic-symbol=pthread_create -Wl,--section-start=.proxy_gateway=0x600000300000 \
  -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
  "$build_source"/{loader.c,proxy.c,vfs.c,links.c,net.c,sysvipc.c,state.c} \
  "$build_source"/adopt-map.c "$build_source"/arch/x86_64/thread-adopt.c \
  "$build_source"/arch/x86_64/{raw.S,adopt.S,altstack.S} -pthread -ldl -lEGL -lGLESv2 \
  -o "$output/libmatonos-v3-loader.so"

MATON_I386_GPU_BRIDGE=1 bash "$build_source/build-i386.sh" "$output/libmatonos-v3-loader-i386.so"

# Freestanding ELF64 peer loader. The i386 client hands x86_64 execs (bash,
# steamwebhelper, driver queries) to the peer loader through a kernel exec;
# that exec inherits the x86_64-native payload filter from the launcher chain,
# which TRAPs bionic startup syscalls (mmap, openat, ...) before the resident
# can install its own SIGSYS handler (exec discards handlers), killing the
# fresh resident silently. A freestanding loader starts at its own entry and
# makes only gateway-exempt raw syscalls until proxy_install runs, so the
# inherited TRAPs never fire. The bionic loader stays the v3 launch path where
# GPU thread adoption needs it.
static_builtins=$("$compiler" --print-libgcc-file-name)
"$compiler" -m64 -march=x86-64-v2 -mcmodel=large "${counter_flags[@]}" \
  -DPROXY_ARCH_BACKEND='"x86_64/backend.h"' \
  -nostdlib -static -Wl,-no-pie -Wl,--image-base=0x600000000000 \
  -Wl,--section-start=.proxy_gateway=0x600000300000 \
  -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
  "$build_source"/{loader.c,proxy.c,vfs.c,links.c,net.c,sysvipc.c,state.c} \
  "$build_source"/arch/x86_64/raw.S "$build_source"/arch/x86_64/entry.S \
  "$static_builtins" -o "$output/libmatonos-v3-loader-static.so"

chmod 0755 "$output/libmatonos-v3-loader.so"
chmod 0755 "$output/libmatonos-v3-loader-static.so"


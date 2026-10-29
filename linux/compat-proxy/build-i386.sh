#!/usr/bin/env bash
# Freestanding ELF32; the NDK supplies headers even without host multilib dev.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
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
  link_flags=(-DMATON_GPU_BRIDGE -fPIC -shared -Wl,-Bsymbolic -Wl,-z,now -Wl,--export-dynamic-symbol=pthread_create)
  backend_sources=(
    "$source_dir/arch/i386/sysinfo.S"
    "$source_dir/arch/i386/sysinfo.c"
    "$source_dir/arch/i386/adopt.S"
    "$source_dir/arch/i386/thread-adopt.c"
    -pthread -ldl -llog -lEGL -lGLESv2
  )
fi
peer_output=$output
if [[ ${MATON_I386_GPU_BRIDGE:-0} == 1 ]]; then peer_output=$(dirname "$output")/libmatonos-v3-peer-i386.so; fi
"$compiler" -m32 -march=i686 -msse2 "${counter_flags[@]}" -DPROXY_ARCH_BACKEND='"i386/backend.h"' \
  "${link_flags[@]}" -Wl,--image-base=0x70000000 -Wl,--section-start=.proxy_gateway=0x70300000 \
  -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
  "$source_dir"/{loader.c,proxy.c,vfs.c,links.c,net.c,sysvipc.c,state.c} \
  "$source_dir"/arch/i386/raw.S "${backend_sources[@]}" "$builtins" -Wl,-soname,libmatonos-v3-peer-i386.so -o "$peer_output"

if [[ ${MATON_I386_GPU_BRIDGE:-0} == 1 ]]; then
  "$compiler" -nostdlib -fPIC -pie -Wl,--no-dynamic-linker \
    -Wl,--image-base=0x70000000 -Wl,--section-start=.proxy_gateway=0x70300000 \
    -Wl,--no-as-needed -Wl,-rpath,'$ORIGIN' -L"$(dirname "$output")" -l:libmatonos-v3-peer-i386.so \
    -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
    "$source_dir/arch/i386/peer-start.c" "$source_dir/arch/i386/peer-start.S" -o "$output"
  # Pin the gateway across kernel exec. The bootstrap changes only its
  # private header back to ET_DYN before entering the stock bionic linker.
  python3 - "$output" <<'PYCODE'
import sys
with open(sys.argv[1], 'r+b') as f:
    f.seek(16)
    if f.read(2) != b'\x03\x00':
        raise SystemExit('peer bootstrap is not ELF ET_DYN')
    f.seek(16)
    f.write(b'\x02\x00')
PYCODE
fi
chmod 0755 "$output" "$peer_output"

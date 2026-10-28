#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external output directory}
headers=${2:?Vulkan-Headers source}
aosp=${MATON_AOSP_ROOT:-/mnt/data/aosp}
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang
runtime=$aosp/out/target/product/pc_x86_64/apex/com.android.runtime
mkdir -p "$out"
out=$(realpath "$out")
MATON_V3_ANDROID_OUT="$out/android" bash "$root/build-android.sh"
bash "$root/../vulkan-forward/build.sh" "$out/vulkan" "$headers"
cc -O2 -Wall -Wextra -Werror "$root/tests/bionic64-start.c" -o "$out/start"
"$compiler" -shared -fPIC -O2 -Wall -Wextra -Werror -I"$headers/include" \
 "$root/tests/bionic32-gpu-provider.c" -ldl -o "$out/provider.so"
cc -O2 -Wall -Wextra -Werror -I"$headers/include" "$root/tests/bionic32-gpu.c" -ldl -o "$out/payload"
"$compiler" -nostdlib -fPIC -pie -mcmodel=large -Wl,--no-dynamic-linker \
 -Wl,--image-base=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 \
 -Wl,--no-as-needed -Wl,-rpath,"$out/android" -L"$out/android" -l:libmatonos-v3-peer-x86_64.so \
 -DPEER_TEST_LINKER=\"$runtime/bin/linker64\" -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
 "$root/arch/x86_64/peer-start.c" "$root/arch/x86_64/peer-start.S" -o "$out/peer"
python3 - "$out/peer" <<'PY'
import sys
with open(sys.argv[1],'r+b') as f:
 f.seek(16);assert f.read(2)==b'\x03\x00';f.seek(16);f.write(b'\x02\x00')
PY
cc -m32 -nostdlib -static -Wl,-e,_start "$root/tests/bionic64-cross.S" -o "$out/cross"
unshare -Urpf --mount-proc env \
 LD_LIBRARY_PATH="$aosp/out/target/product/pc_x86_64/system/lib64:$runtime/lib64/bionic" \
 MATON_TEST_GPU_PROVIDER="$out/provider.so" \
 timeout 30 "$out/start" "$out/cross" "$out/peer" --gpu-bridge "$out/payload" "$out/vulkan/libmaton_vulkan.so"

set +e
unshare -Urpf --mount-proc env \
 LD_LIBRARY_PATH="$aosp/out/target/product/pc_x86_64/system/lib64:$runtime/lib64/bionic" \
 MATON_TEST_GPU_PROVIDER="$out/provider.so" MATON_TEST_NO_NATIVE_EXEMPT=1 \
 timeout 30 "$out/start" "$out/cross" "$out/peer" --gpu-bridge "$out/payload" "$out/vulkan/libmaton_vulkan.so" \
 >"$out/no-exemption.log" 2>&1
negative_status=$?
set -e
if (( negative_status != 159 )); then
 echo "FAIL expected ELF64 linker SIGSYS without native exemption, got $negative_status" >&2
 exit 1
fi
echo 'PASS negative control: ELF64 bionic startup dies with SIGSYS without inherited native exemption'

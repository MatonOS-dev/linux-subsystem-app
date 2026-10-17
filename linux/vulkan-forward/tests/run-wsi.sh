#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external output directory}
headers=${2:?pinned Vulkan-Headers v1.4.341 source}
# build.sh validates output location before any test output is written.
MATON_VK_WSI_PROTOTYPE=1 bash "$here/build.sh" "$out" "$headers"
cc=${CC:-clang-21}
flags=(-std=c11 -g -Wall -Wextra -Werror -fno-builtin -fno-stack-protector -DMV_WSI_PROTOTYPE=1 -I"$here" -I"$here/arch/x86_64" -I"$headers/include")
"$cc" "${flags[@]}" -fsanitize=address,undefined "$here/runtime.c" "$here/wsi.c" "$here/tests/wsi-test.c" -o "$out/wsi-test"
"$out/wsi-test"
"$cc" "${flags[@]}" -fsanitize=address,undefined "$here/tests/wsi-wayland-test.c" -o "$out/wsi-wayland-test"
"$out/wsi-wayland-test"
"$cc" "${flags[@]}" -I"$here" "$here/runtime.c" "$here/wayland_resolve.c" "$here/tests/wayland-resolve-test.c" -ldl -o "$out/wayland-resolve-test"
"$out/wayland-resolve-test"
"$cc" "${flags[@]}" -UMV_WSI_PROTOTYPE "$here/runtime.c" "$here/wsi.c" "$here/tests/wsi-disabled.c" -o "$out/wsi-disabled"
"$out/wsi-disabled"
for arch in aarch64 riscv64; do
  "$cc" --target="$arch-linux-gnu" -std=c11 -O2 -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -DMV_WSI_PROTOTYPE=1 -I"$headers/include" -I"$here" -c "$here/wsi.c" -o "$out/wsi-enabled-$arch.o"
done
"$cc" "${flags[@]}" -I"$headers/include" "$here/tests/query-test.c" -ldl -o "$out/query-test"
env -u MATON_GPU_BRIDGE -u MATON_THREAD_ADOPT "$out/query-test" "$out/libmaton_vulkan.so"
python3 - "$out/libmaton_vulkan.so" <<'PY'
import subprocess, sys
s=subprocess.check_output(['llvm-readelf-21','--dyn-syms',sys.argv[1]],text=True)
exports={l.split()[-1] for l in s.splitlines() if 'GLOBAL DEFAULT' in l}
assert exports=={'vk_icdGetInstanceProcAddr','vk_icdGetPhysicalDeviceProcAddr','vk_icdNegotiateLoaderICDInterfaceVersion'},exports
assert not [l for l in s.splitlines() if ' UND ' in l and ('GLOBAL' in l or 'WEAK' in l)]
print('PASS prototype DSO: three exports, zero undefined; enabled WSI cross-compiles for arm64/riscv64')
PY

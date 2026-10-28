#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external build directory}
headers=${2:?Vulkan-Headers v1.4.341 source}
bash "$here/build.sh" "$out" "$headers"
cc=${CC:-clang-21}
flags=(-std=c11 -O2 -Wall -Wextra -Werror -fno-builtin -fno-stack-protector -I"$here" -I"$here/arch/x86_64")
"$cc" "${flags[@]}" "$here/runtime.c" "$here/tests/runtime-test.c" -pthread -o "$out/runtime-test"
"$out/runtime-test"
"$cc" "${flags[@]}" "$here/tests/bootstrap-scan-test.c" -o "$out/bootstrap-scan-test"
"$out/bootstrap-scan-test"
"$cc" "${flags[@]}" -I"$headers/include" "$here/tests/bootstrap-fork-test.c" -ldl -o "$out/bootstrap-fork-test"
VK_DRIVER_FILES=${MATON_TEST_VK_DRIVER:-/usr/share/vulkan/icd.d/lvp_icd.json} "$out/bootstrap-fork-test" "$out/libmaton_vulkan.so"
"$cc" "${flags[@]}" "$here/tests/abi-test.c" -o "$out/abi-test"
"$out/abi-test"
"$cc" -m32 -march=corei7 -std=c11 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
  -I"$here" -c "$here/tests/abi-i386-test.c" -o "$out/abi-i386-test.o"
echo 'PASS i386 adoption API and GS scope ABI offsets'
"$cc" "${flags[@]}" -I"$headers/include" "$here/tests/query-test.c" -ldl -o "$out/query-test"
env -u MATON_GPU_BRIDGE -u MATON_THREAD_ADOPT "$out/query-test" "$out/libmaton_vulkan.so"
"$cc" "${flags[@]}" "$here/tests/switch-bench.c" "$here/runtime.c" -o "$out/switch-bench"
bash "$here/tests/switch-bench.sh" "$out"
python3 - "$out" <<'PY'
import pathlib, subprocess, sys
out=pathlib.Path(sys.argv[1])
symbols=subprocess.check_output(['llvm-readelf-21','--dyn-syms',str(out/'libmaton_vulkan.so')],text=True)
exports={s.split()[-1] for s in symbols.splitlines() if 'GLOBAL DEFAULT' in s}
assert exports=={'vk_icdGetInstanceProcAddr','vk_icdGetPhysicalDeviceProcAddr','vk_icdNegotiateLoaderICDInterfaceVersion'},exports
undefined=[s.split()[-1] for s in symbols.splitlines() if ' UND ' in s and ('WEAK' in s or 'GLOBAL' in s)]
assert undefined==[],undefined
print('PASS: three ICD exports; zero undefined symbols')
PY

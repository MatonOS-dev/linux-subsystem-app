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
"$cc" "${flags[@]}" "$here/tests/abi-test.c" -o "$out/abi-test"
"$out/abi-test"
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

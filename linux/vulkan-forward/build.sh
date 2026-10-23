#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh ABSOLUTE_OUTPUT_DIR VULKAN_HEADERS_SOURCE}
headers=${2:?pinned Vulkan-Headers v1.4.341 source required}
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
case "$out/" in "$here/"*|"$(git -C "$here" rev-parse --show-toplevel)/"*) echo 'output must be outside repository' >&2; exit 2;; esac
mkdir -p "$out"
python3 "$here/gen.py" "$headers/registry/vk.xml" "$out"
cc=${CC:-clang-21}
flags=(-std=c11 -O2 -march=x86-64-v2 -g -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -I"$headers/include" -I"$out" -I"$here" -I"$here/arch/x86_64")
"$cc" "${flags[@]}" -shared -Wl,--no-undefined,-Bsymbolic,-z,now,-z,noexecstack "$here/runtime.c" "$here/icd.c" "$here/wsi.c" "$here/wsi_wayland.c" "$here/wayland_resolve.c" -o "$out/libmaton_vulkan.so"
readelf=${READELF:-llvm-readelf-21}
check_icd() {
  local lib=$1 label=$2
  "$readelf" -d "$lib" > "$out/dynamic-$label.txt"
  if grep -E -q NEEDED "$out/dynamic-$label.txt"; then echo "$label unexpected DT_NEEDED" >&2;exit 1;fi
  "$readelf" --dyn-syms "$lib" > "$out/symbols-$label.txt"
  python3 - "$out/symbols-$label.txt" <<'PY'
import pathlib,sys
symbols=pathlib.Path(sys.argv[1]).read_text().splitlines()
exports={line.split()[-1] for line in symbols if 'GLOBAL DEFAULT' in line}
expected={'vk_icdGetInstanceProcAddr','vk_icdGetPhysicalDeviceProcAddr','vk_icdNegotiateLoaderICDInterfaceVersion'}
assert exports==expected,(exports,expected)
undefined=[line for line in symbols if ' UND ' in line and ('WEAK' in line or 'GLOBAL' in line)]
assert not undefined,undefined
PY
}
check_icd "$out/libmaton_vulkan.so" x86_64
mkdir -p "$out/i386"
i386_flags=(-m32 -march=corei7 -std=c11 -O2 -g -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -I"$headers/include" -I"$out" -I"$here" -I"$here/arch/i386")
"$cc" "${i386_flags[@]}" -shared -Wl,--no-undefined,-Bsymbolic,-z,now,-z,noexecstack "$here/runtime.c" "$here/icd.c" "$here/wsi.c" "$here/wsi_wayland.c" "$here/wayland_resolve.c" -o "$out/i386/libmaton_vulkan.so"
check_icd "$out/i386/libmaton_vulkan.so" i386
for arch in aarch64 riscv64; do
  for unit in runtime icd wsi wsi_wayland wayland_resolve; do
    "$cc" --target="$arch-linux-gnu" -std=c11 -O2 -ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror -I"$headers/include" -I"$out" -I"$here" -I"$here/arch/$arch" -c "$here/$unit.c" -o "$out/$unit-$arch.o"
  done
done
"$readelf" -h "$out/i386/libmaton_vulkan.so" | grep -E -q 'Class:.*ELF32'
sed "s|@GL_EXTENSION_DIR@|$out|;s|/lib/libmaton|/libmaton|" "$here/../gpu-forward-spike/flatpak/maton_icd.json.in" > "$out/maton_icd.json"
cat "$out/coverage.txt"
echo 'PASS: x86_64 and i386 ICDs have three exports, no undefined symbols or DT_NEEDED; aarch64/riscv64 core compiled (stub backends)'

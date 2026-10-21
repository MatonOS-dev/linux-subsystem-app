#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh ABSOLUTE_OUTPUT_DIR DEPS_DIR ANDROID_NDK_R30}
deps=${2:?pinned libglvnd ABI headers required}
ndk=${3:?Android NDK r30 required}
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
case "$out/" in "$here/"*|"$(git -C "$here" rev-parse --show-toplevel)/"*) echo 'output must be outside repository' >&2; exit 2;; esac
mkdir -p "$out"
cc=${CC:-clang-21}; readelf=${READELF:-llvm-readelf-21}
headers="$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include"
"$cc" --target=x86_64-linux-android35 -std=c11 -O2 -g -ffreestanding -nostdlib \
  -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror \
  -I"$here/include" -I"$deps/include" -I"$headers/x86_64-linux-android" -I"$headers" \
  -shared -Wl,-Bsymbolic,-z,now,-z,noexecstack "$here/vendor.c" -o "$out/libGLX_maton.so.0"
"$readelf" -d "$out/libGLX_maton.so.0" > "$out/dynamic.txt"
if rg -q NEEDED "$out/dynamic.txt"; then echo 'unexpected DT_NEEDED' >&2; exit 1; fi
"$readelf" --dyn-syms "$out/libGLX_maton.so.0" > "$out/symbols.txt"
exports=$(awk '$4 == "FUNC" && $5 == "GLOBAL" && $7 != "UND" { print $8 }' "$out/symbols.txt" | sed 's/@.*//' | sort -u)
[[ "$exports" == __glx_Main ]] || { echo "unexpected exports: $exports" >&2; exit 1; }
ln -sf libGLX_maton.so.0 "$out/libGLX_maton.so"
echo 'PASS: GLX vendor DSO; no DT_NEEDED; only __glx_Main exported'

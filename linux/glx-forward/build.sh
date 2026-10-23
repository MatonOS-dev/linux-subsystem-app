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
"$cc" --target=x86_64-linux-android35 -march=x86-64-v2 -std=c11 -O2 -g -ffreestanding -nostdlib \
  -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror \
  -I"$here/include" -I"$deps/include" -I"$headers/x86_64-linux-android" -I"$headers" \
  -shared -Wl,-Bsymbolic,-z,now,-z,noexecstack "$here/vendor.c" -o "$out/libGLX_maton.so.0"
"$readelf" -d "$out/libGLX_maton.so.0" > "$out/dynamic.txt"
if grep -E -q NEEDED "$out/dynamic.txt"; then echo 'unexpected DT_NEEDED' >&2; exit 1; fi
"$readelf" --dyn-syms "$out/libGLX_maton.so.0" > "$out/symbols.txt"
exports=$(awk '$4 == "FUNC" && $5 == "GLOBAL" && $7 != "UND" { print $8 }' "$out/symbols.txt" | sed 's/@.*//' | sort -u)
[[ "$exports" == __glx_Main ]] || { echo "unexpected exports: $exports" >&2; exit 1; }
ln -sf libGLX_maton.so.0 "$out/libGLX_maton.so"
mkdir -p "$out/i386"
"$cc" --target=i686-linux-android35 -march=corei7 -std=c11 -O2 -g -ffreestanding -nostdlib \
  -fno-stack-protector -fno-builtin -fvisibility=hidden -fPIC -Wall -Wextra -Werror \
  -I"$here/include" -I"$deps/include" -I"$headers/i686-linux-android" -I"$headers" \
  -shared -Wl,-Bsymbolic,-z,now,-z,noexecstack "$here/vendor.c" -o "$out/i386/libGLX_maton.so.0"
"$readelf" -d "$out/i386/libGLX_maton.so.0" > "$out/i386/dynamic.txt"
if grep -E -q NEEDED "$out/i386/dynamic.txt"; then echo 'i386 unexpected DT_NEEDED' >&2; exit 1; fi
"$readelf" --dyn-syms "$out/i386/libGLX_maton.so.0" > "$out/i386/symbols.txt"
i386_exports=$(awk '$4 == "FUNC" && $5 == "GLOBAL" && $7 != "UND" { print $8 }' "$out/i386/symbols.txt" | sed 's/@.*//' | sort -u)
[[ "$i386_exports" == __glx_Main ]] || { echo "unexpected i386 exports: $i386_exports" >&2; exit 1; }
"$readelf" -h "$out/i386/libGLX_maton.so.0" > "$out/i386/header.txt"
grep -E -q \'Class:.*ELF32\' "$out/i386/header.txt"
ln -sf libGLX_maton.so.0 "$out/i386/libGLX_maton.so"
echo 'PASS: x86_64 and i386 GLX vendor DSOs; no DT_NEEDED; only __glx_Main exported'

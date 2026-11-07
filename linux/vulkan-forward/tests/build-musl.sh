#!/usr/bin/env bash
# Dynamic musl process and dynamic pinned Khronos loader; ICD unchanged.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?external output}; deps=${2:?stage-A dependency output (src and deps)}
case "$out" in /*) ;; *) exit 2;; esac
repo=$(git -C "$here" rev-parse --show-toplevel)
case "$(realpath -m "$out")/" in "$repo/"*) echo 'output must be external' >&2; exit 2;; esac
mkdir -p "$out/src" "$out/tests"
archive="$out/src/musl.tar.gz"
if [[ ! -f "$archive" ]]; then curl -fL --retry 2 --connect-timeout 15 --max-time 180 https://musl.libc.org/releases/musl-1.2.6.tar.gz -o "$archive"; fi
echo "e9db8b70b0cba3db43b27e84ccd8a0c0958b40029a1c36c9f13817b93787b4d6  $archive" | sha256sum -c -
if [[ ! -d "$out/src/musl-1.2.6" ]]; then tar xf "$archive" -C "$out/src"; fi
(cd "$out/src/musl-1.2.6"; ./configure --prefix="$out/musl" --syslibdir="$out/musl/lib"; make -j4; make install)
cmake=${CMAKE:-cmake}
"$cmake" -S "$deps/src/Vulkan-Loader-1.4.341" -B "$out/musl-loader-build" \
 -DCMAKE_C_COMPILER="$out/musl/bin/musl-gcc" -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_EXE_LINKER_FLAGS="-Wl,--dynamic-linker=$out/musl/lib/libc.so" \
 -DCMAKE_PREFIX_PATH="$deps/deps" -DBUILD_TESTS=OFF -DBUILD_WSI_XCB_SUPPORT=OFF \
 -DBUILD_WSI_XLIB_SUPPORT=OFF -DBUILD_WSI_WAYLAND_SUPPORT=OFF -DBUILD_WSI_DIRECTFB_SUPPORT=OFF -DUPDATE_DEPS=OFF
"$cmake" --build "$out/musl-loader-build" -j4
# Generate the same embedded SPIR-V as the glibc test, with its existing builder.
bash "$here/build-offscreen.sh" "$out/tests" "$deps/src/Vulkan-Headers-1.4.341" "$deps/loader-build"
"$out/musl/bin/musl-gcc" -std=c11 -O2 -g -Wall -Wextra -Werror '-DMATON_TEST_LIBC="musl 1.2.6"' \
 -I"$out/tests" -I"$deps/src/Vulkan-Headers-1.4.341/include" "$here/offscreen.c" \
 -L"$out/musl-loader-build/loader" -lvulkan -pthread -o "$out/tests/offscreen-musl"
readelf -d "$out/musl-loader-build/loader/libvulkan.so.1.4.341"

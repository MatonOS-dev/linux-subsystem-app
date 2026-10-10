#!/usr/bin/env bash
set -euo pipefail

STUBGEN_DIR=$(cd "$(dirname "$0")/.." && pwd)
NDK_CLANG=${NDK_CLANG:-${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang}
if [[ ! -x "$NDK_CLANG" ]]; then
  echo "Set ANDROID_NDK_HOME or NDK_CLANG to an Android NDK clang" >&2
  exit 1
fi

build_marker() {
  local abi=$1 target=$2 api=$3
  local output="$STUBGEN_DIR/resources/lib/$abi/libmatonos-abi-marker.so"
  mkdir -p "$(dirname "$output")"
  "$NDK_CLANG" --target="$target-linux-android$api" -fPIC -shared -nostdlib \
    -Wl,-soname,libmatonos-abi-marker.so -Wl,--build-id=none -Os \
    -o "$output" "$STUBGEN_DIR/native/abi_marker.c"
}

build_marker x86_64 x86_64 30
build_marker arm64-v8a aarch64 30
build_marker riscv64 riscv64 35

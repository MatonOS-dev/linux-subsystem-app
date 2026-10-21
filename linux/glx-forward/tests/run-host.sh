#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?output directory required}
mkdir -p "$OUT"
cc=${CC:-cc}
"$cc" -std=c11 -O2 -fPIC -shared -I"$ROOT/glx-forward/include" \
  -I"$ROOT/glx-forward/deps/include" "$ROOT/glx-forward/vendor.c" \
  -o "$OUT/libGLX_maton-host.so" -lEGL -lGLESv2 -lX11
"$cc" -std=c11 -O2 -I"$ROOT/glx-forward/include" \
  -I"$ROOT/glx-forward/deps/include" "$ROOT/glx-forward/tests/abi.c" \
  -o "$OUT/abi-test" -ldl
"$OUT/abi-test" "$OUT/libGLX_maton-host.so"
"$cc" -std=c11 -O2 -I"$ROOT/glx-forward/include" \
  "$ROOT/glx-forward/tests/swap.c" -o "$OUT/swap-test" -lGLESv2 -lX11 -ldl
"$OUT/swap-test"

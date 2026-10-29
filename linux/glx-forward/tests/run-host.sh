#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?output directory required}
mkdir -p "$OUT"
deps=${2:-$ROOT/glx-forward/deps}
cc=${CC:-cc}
"$cc" -std=c11 -O2 -fPIC -shared -I"$ROOT/glx-forward/include" \
  -I"$deps/include" "$ROOT/glx-forward/vendor.c" \
  -o "$OUT/libGLX_maton-host.so" -lEGL -lGLESv2 -lX11
"$cc" -std=c11 -O2 -I"$ROOT/glx-forward/include" \
  -I"$deps/include" "$ROOT/glx-forward/tests/abi.c" \
  -o "$OUT/abi-test" -ldl
"$OUT/abi-test" "$OUT/libGLX_maton-host.so"
"$cc" -std=c11 -O2 -I"$ROOT/glx-forward/include" \
  "$ROOT/glx-forward/tests/swap.c" -o "$OUT/swap-test" -lGLESv2 -lX11 -ldl
"$OUT/swap-test"
# Keep the on-device depth/stencil regression probe buildable on developer hosts.
"$cc" -std=c11 -O2 "$ROOT/glx-forward/tests/depth-stencil.c" \
  -o "$OUT/depth-stencil-test" -lGL -lX11
"$cc" -std=c11 -O2 "$ROOT/glx-forward/tests/atlas2.c" \
  -o "$OUT/atlas2-test" -lGL -lX11

if command -v xvfb-run >/dev/null; then
  mkdir -p "$OUT/mesa-direct"
  "$cc" -std=c11 -O2 -fPIC -shared -I"$ROOT/glx-forward/include" "$ROOT/glx-forward/tests/mesa-direct.c" -lEGL -o "$OUT/mesa-direct/libEGL_maton.so"
  "$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$ROOT/glx-forward/include" -I"$deps/include" "$ROOT/glx-forward/tests/visual.c" -lX11 -ldl -o "$OUT/visual-test"
  EGL_PLATFORM=surfaceless LD_LIBRARY_PATH="$OUT/mesa-direct${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" xvfb-run -a "$OUT/visual-test"
else
  echo 'SKIP GLX visual integration test: Xvfb required'
fi

#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: run-host.sh ABSOLUTE_OUTPUT_DIR EGL_FORWARD_BUILD}
build=${2:?forwarder build output required}
deps=${3:?pinned dependencies required}
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
mkdir -p "$out"
cc=${CC:-cc}
"$cc" -I"$deps/include" "$here/tests/abi-test.c" -ldl -o "$out/abi-test"
"$out/abi-test" "$build/libEGL_maton.so"
"$cc" -I"$deps/include" "$here/tests/offscreen.c" -Wl,-l:libEGL.so.1 -Wl,-l:libGLESv2.so.2 -ldl -o "$out/offscreen"
"$cc" -I"$deps/include" "$here/tests/desktop.c" -ldl -o "$out/desktop-glibc"
if [[ -n ${MUSL_CC:-} ]]; then
  "$MUSL_CC" -I"$deps/include" "$here/tests/desktop.c" -ldl -o "$out/desktop-musl"
elif command -v musl-gcc >/dev/null 2>&1; then
  musl-gcc -I"$deps/include" "$here/tests/desktop.c" -ldl -o "$out/desktop-musl"
else
  echo 'SKIP: musl desktop harness (set MUSL_CC or install musl-gcc)' >&2
fi
"$cc" "$here/tests/switch-bench.c" -o "$out/switch-bench"
"$out/switch-bench"
echo "Built offscreen harness at $out/offscreen and desktop harness at $out/desktop-glibc${MUSL_CC:+ and $out/desktop-musl}; run them through the adopted Android EGL bridge, not host Mesa GLVND."

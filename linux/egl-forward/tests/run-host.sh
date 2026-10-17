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
"$cc" "$here/tests/switch-bench.c" -o "$out/switch-bench"
"$out/switch-bench"
echo "Built offscreen harness at $out/offscreen; run it through the adopted Android EGL bridge, not host Mesa GLVND."

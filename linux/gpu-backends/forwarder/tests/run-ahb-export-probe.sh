#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?absolute output directory}
ndk=${2:?Android NDK r30 directory}
headers=${3:?pinned Vulkan-Headers v1.4.341 source}
adb=${4:-adb}
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
mkdir -p "$out"
clang="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
"$clang" --target=x86_64-linux-android35 -fPIE -pie -Wall -Wextra -Werror \
  -I"$headers/include" "$here/tests/ahb-export-probe.c" \
  -lvulkan -landroid -lnativewindow -o "$out/ahb-vulkan-probe"
"$adb" -s localhost:5556 shell mkdir -p /data/local/tmp/wsi
"$adb" -s localhost:5556 push "$out/ahb-vulkan-probe" /data/local/tmp/wsi/ahb-vulkan-probe
"$adb" -s localhost:5556 shell chmod 755 /data/local/tmp/wsi/ahb-vulkan-probe
"$adb" -s localhost:5556 shell /data/local/tmp/wsi/ahb-vulkan-probe

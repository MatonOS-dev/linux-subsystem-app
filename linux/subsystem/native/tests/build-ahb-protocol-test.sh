#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external output}; deps=${2:?private compositor dependencies}
ndk=${MATON_NDK:-$HOME/Documents/android-ndk-r30}
cc=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
repo=$(git -C "$here" rev-parse --show-toplevel)
case "$(realpath -m "$out")/" in "$repo/"*) echo 'output must be external' >&2; exit 2;; esac
mkdir -p "$out"
"$deps/host/bin/wayland-scanner" client-header "$here/protocol/maton-ahb-v1.xml" "$out/maton-ahb-v1-client-protocol.h"
"$deps/host/bin/wayland-scanner" server-header "$here/protocol/maton-ahb-v1.xml" "$out/maton-ahb-v1-server-protocol.h"
"$deps/host/bin/wayland-scanner" private-code "$here/protocol/maton-ahb-v1.xml" "$out/maton-ahb-v1.c"
"$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -DWLR_USE_UNSTABLE \
 -I"$here" -I"$out" -I"$deps/stage/x86_64/include" -I"$deps/stage/x86_64/include/pixman-1" \
 -I"$deps/sources/src/wlroots-0.20.2/include" \
 "$here/tests/ahb_protocol_test.c" "$here/ahb_protocol.c" "$here/maton_renderer.c" "$out/maton-ahb-v1.c" \
 -L"$deps/stage/x86_64/lib" -L"$deps/xstage/x86_64/lib" \
 -Wl,-rpath-link,"$deps/stage/x86_64/lib:$deps/xstage/x86_64/lib" \
 -lwlroots-0.20 -lwayland-server -lwayland-client -landroid -o "$out/ahb-protocol-test"

if [[ -n ${3:-} ]]; then
  headers=$3
  ${CC:-clang-21} -std=c11 -O2 -g -Wall -Wextra -Werror -DMATON_AHB_LINUX_CLIENT -DMV_WSI_PROTOTYPE=1 -fno-builtin \
   -I"$here" -I"$here/../../vulkan-forward" -I"$here/../../vulkan-forward/arch/x86_64" \
   -I"$here/../../gpu-forward-spike" -I"$headers/include" -I"$out" -I"$deps/stage/x86_64/include" \
   "$here/tests/ahb_protocol_test.c" "$here/../../vulkan-forward/wsi.c" "$here/../../vulkan-forward/runtime.c" "$out/maton-ahb-v1.c" \
   -Wl,-l:libwayland-client.so.0 -o "$out/ahb-linux-client"
fi

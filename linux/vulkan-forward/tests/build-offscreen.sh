#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?external output directory}; headers=${2:?Vulkan-Headers source}; loader=${3:?loader build directory}
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
repo=$(git -C "$here" rev-parse --show-toplevel)
case "$(realpath -m "$out")/" in "$repo/"*) echo 'output must be outside repository' >&2; exit 2;; esac
mkdir -p "$out"
glslangValidator -V "$here/compute.comp" -o "$out/compute.spv"
glslangValidator -V "$here/triangle.vert" -o "$out/vertex.spv"
glslangValidator -V "$here/triangle.frag" -o "$out/fragment.spv"
python3 - "$out" <<'PY'
import pathlib,struct,sys
out=pathlib.Path(sys.argv[1]);text='#include <stdint.h>\n'
for name in ['compute','vertex','fragment']:
 data=(out/(name+'.spv')).read_bytes();words=struct.unpack('<'+'I'*(len(data)//4),data)
 text+='static const uint32_t '+name+'_spv[]={'+','.join(hex(x) for x in words)+'};\n'
(out/'shaders.h').write_text(text)
PY
clang-21 -std=c11 -O2 -g -Wall -Wextra -Werror -I"$out" -I"$headers/include" "$here/offscreen.c" -L"$loader/loader" -lvulkan -pthread -o "$out/offscreen"

#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
deps=${1:?usage: fetch-tools.sh ABSOLUTE_DEPS_DIR}
case "$deps" in /*) ;; *) echo 'dependency output must be absolute' >&2; exit 2;; esac
case "$deps/" in "$here/"*|"$(git -C "$here" rev-parse --show-toplevel)/"*) echo 'dependencies must be outside repository' >&2; exit 2;; esac
pins="$here/pins.json"
mkdir -p "$deps/include/glvnd"
fetch() {
  local url=$1 out=$2 sha=$3
  curl --fail --location --silent --show-error "$url" -o "$out"
  local got
  got=$(sha256sum "$out" | cut -d' ' -f1)
  [[ $got == "$sha" ]] || { echo "hash mismatch for $out: $got" >&2; exit 1; }
}
egl_commit=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["EGL-Registry"]["commit"])' "$pins")
gl_commit=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["OpenGL-Registry"]["commit"])' "$pins")
glvnd_commit=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["libglvnd"]["commit"])' "$pins")
fetch "https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/$egl_commit/api/egl.xml" "$deps/egl.xml" "$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["EGL-Registry"]["sha256"])' "$pins")"
fetch "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/$gl_commit/xml/gl.xml" "$deps/gl.xml" "$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["OpenGL-Registry"]["sha256"])' "$pins")"
for file in include/glvnd/libeglabi.h include/glvnd/GLdispatchABI.h; do
  hash=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["libglvnd"][sys.argv[2]])' "$pins" "$file")
  fetch "https://raw.githubusercontent.com/NVIDIA/libglvnd/$glvnd_commit/$file" "$deps/$file" "$hash"
done

#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
deps=${1:?usage: fetch-tools.sh ABSOLUTE_DEPS_DIR}
case "$deps" in /*) ;; *) echo 'dependency output must be absolute' >&2; exit 2;; esac
case "$deps/" in "$here/"*|"$(git -C "$here" rev-parse --show-toplevel)/"*) echo 'dependencies must be outside repository' >&2; exit 2;; esac
pins="$here/pins.json"
mkdir -p "$deps/include/glvnd"
for file in include/glvnd/libglxabi.h include/glvnd/GLdispatchABI.h; do
  hash=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["libglvnd"][sys.argv[2]])' "$pins" "$file")
  curl --fail --location --silent --show-error "https://raw.githubusercontent.com/NVIDIA/libglvnd/$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["libglvnd"]["commit"])' "$pins")/$file" -o "$deps/$file"
  got=$(sha256sum "$deps/$file" | cut -d' ' -f1)
  [[ $got == "$hash" ]] || { echo "hash mismatch for $file: $got" >&2; exit 1; }
done

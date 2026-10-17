#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external output directory}
headers=${2:?compositor Wayland include directory}
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
repo=$(git -C "$here" rev-parse --show-toplevel)
case "$(realpath -m "$out")/" in "$repo/"*) echo 'output must be external' >&2; exit 2;; esac
mkdir -p "$out"
${CC:-clang-21} -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I"$here" -I"$headers" "$here/buffer_release.c" "$here/tests/buffer_release_test.c" \
 -pthread -Wl,-l:libwayland-server.so.0,--wrap=wl_event_loop_add_fd -o "$out/buffer-release-test"
"$out/buffer-release-test"

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
loader=${COMPAT_PROXY:-$PWD/build/linux-layer-v3}
fixture=$(mktemp -d "$PWD/build/exec-relative.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
cc -O2 -Wall -Wextra -Werror tests/exec-relative.c -o "$fixture/image"
ln -s image "$fixture/multicall-alias"
(cd "$fixture"; timeout 20 "$loader" --android-filter ./multicall-alias child)
(cd "$fixture"; timeout 20 "$loader" --android-filter ./image start)
(cd "$fixture"; timeout 20 "$loader" --android-filter --map "$fixture" /relative-image /bin/sh -c 'cd /relative-image; exec ./image start')

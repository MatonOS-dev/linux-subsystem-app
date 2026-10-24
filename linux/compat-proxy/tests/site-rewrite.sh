#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -O2 -Wall -Wextra -Werror -pthread tests/site-rewrite.c tests/site-rewrite.S -ldl -o build/site-rewrite
cc -no-pie -O2 -Wall -Wextra -Werror -pthread tests/site-rewrite.c tests/site-rewrite.S -ldl -o build/site-rewrite-exec
cc -shared -fPIC tests/site-rewrite.S -o build/site-rewrite.so
for binary in site-rewrite site-rewrite-exec; do
    for enabled in 0 1; do
        MATON_PROXY_REWRITE=$enabled timeout 60 build/linux-layer-v3 --android-filter \
            "$PWD/build/$binary" "$enabled" "$PWD/build/site-rewrite.so"
    done
done

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -O2 -Wall -Wextra -Werror -pthread tests/site-rewrite.c tests/site-rewrite.S -ldl -o build/site-rewrite
cc -no-pie -O2 -Wall -Wextra -Werror -pthread tests/site-rewrite.c tests/site-rewrite.S -ldl -o build/site-rewrite-exec
cc -shared -fPIC tests/site-rewrite.S -o build/site-rewrite.so
for binary in site-rewrite site-rewrite-exec; do
    for enabled in 0 1 default; do
        if [[ $enabled == default ]]; then
            env -u MATON_PROXY_REWRITE timeout 60 build/linux-layer-v3 --android-filter \
                "$PWD/build/$binary" 1 "$PWD/build/site-rewrite.so"
        else
            MATON_PROXY_REWRITE=$enabled timeout 60 build/linux-layer-v3 --android-filter \
                "$PWD/build/$binary" "$enabled" "$PWD/build/site-rewrite.so"
        fi
    done
done
cc -O2 -Wall -Wextra -Werror tests/site-rewrite-fde.c -ldl -o build/site-rewrite-fde
cc -shared -fPIC tests/site-rewrite-fde.S -o build/site-rewrite-fde.so
strip --strip-all build/site-rewrite-fde.so
MATON_PROXY_REWRITE=1 timeout 60 build/linux-layer-v3 --android-filter \
    "$PWD/build/site-rewrite-fde"
cc -O2 -Wall -Wextra -Werror -pthread tests/site-rewrite-lazy.c tests/site-rewrite-lazy.S -o build/site-rewrite-lazy
for enabled in 0 1; do
    MATON_PROXY_REWRITE=$enabled MATON_PROXY_REWRITE_LOG=1 timeout 60 build/linux-layer-v3 --android-filter \
        "$PWD/build/site-rewrite-lazy" "$enabled" 2>build/site-rewrite-lazy-$enabled.log
done
grep -q 'rewrite-race-recovered' build/site-rewrite-lazy-1.log
grep -q 'rewrite registry path=' build/site-rewrite-lazy-1.log

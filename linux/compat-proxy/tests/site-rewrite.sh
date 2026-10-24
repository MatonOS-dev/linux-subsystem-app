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
cache=$(mktemp -d "$PWD/build/rewrite-cache.XXXXXX")
trap 'rm -rf "$cache"' EXIT
for run in first second; do
    XDG_CACHE_HOME="$cache" MATON_PROXY_REWRITE=1 MATON_PROXY_REWRITE_LOG=1 timeout 60 \
        build/linux-layer-v3 --android-filter "$PWD/build/site-rewrite-lazy" 1 \
        >"build/rewrite-cache-$run.log" 2>&1
done
grep -q rewrite-persistent-hit build/rewrite-cache-second.log
# Tampering only the function checksum forces a re-decode of every entry.
python3 - "$cache" <<'PYCRC'
import pathlib,sys
for p in pathlib.Path(sys.argv[1]).rglob('*'):
    if p.is_file():
        data=bytearray(p.read_bytes());data[128]^=1;p.write_bytes(data)
PYCRC
XDG_CACHE_HOME="$cache" MATON_PROXY_REWRITE=1 MATON_PROXY_REWRITE_LOG=1 timeout 60 \
    build/linux-layer-v3 --android-filter "$PWD/build/site-rewrite-lazy" 1 \
    >build/rewrite-cache-checksum.log 2>&1
if grep -q rewrite-persistent-hit build/rewrite-cache-checksum.log; then exit 1; fi
# A corrupt/truncated entry must be treated as absent, never as a plan.
find "$cache" -type f -exec truncate -s 7 {} +
XDG_CACHE_HOME="$cache" MATON_PROXY_REWRITE=1 MATON_PROXY_REWRITE_LOG=1 timeout 60 \
    build/linux-layer-v3 --android-filter "$PWD/build/site-rewrite-lazy" 1 \
    >build/rewrite-cache-corrupt.log 2>&1
if grep -q rewrite-persistent-hit build/rewrite-cache-corrupt.log; then exit 1; fi

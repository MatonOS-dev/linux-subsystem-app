#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bytes=${1:-1073741824}
mkdir -p build
cc -O2 -Wall -Wextra -Werror tests/trap-bench.c -o build/trap-bench
fixture=$(mktemp -d "$PWD/build/io-bench.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
printf 'compat proxy metadata benchmark\n' >"$fixture/file"
blocks=$(((bytes + 1048575) / 1048576))
dd if=/dev/zero of="$fixture/big" bs=1048576 count="$blocks" conv=fsync status=none

printf 'same-filesystem native/proxied metadata benchmark (10,000 operations)\n'
for mode in open stat statx; do
    for route in native proxied rewrite; do
        if [[ $route == native ]]; then path="$fixture/file"; else path=/bench/file; fi
        printf "route=%s " "$route"
        if [[ $route == native ]]; then
            build/trap-bench "$mode" 10000 "$path"
        else
            env MATON_PROXY_REWRITE=$([[ $route == rewrite ]] && echo 1 || echo 0) build/linux-layer-v3 --android-filter --map "$fixture" /bench \
                build/trap-bench "$mode" 10000 "$path"
        fi
    done
done

printf 'same-filesystem sequential I/O benchmark (%s bytes per operation)\n' "$bytes"
for mode in read mmap; do
    for route in native proxied rewrite; do
        if [[ $route == native ]]; then path="$fixture/big"; else path=/bench/big; fi
        printf "route=%s " "$route"
        if [[ $route == native ]]; then
            build/trap-bench "$mode" "$bytes" "$path"
        else
            env MATON_PROXY_REWRITE=$([[ $route == rewrite ]] && echo 1 || echo 0) build/linux-layer-v3 --android-filter --map "$fixture" /bench \
                build/trap-bench "$mode" "$bytes" "$path"
        fi
    done
done

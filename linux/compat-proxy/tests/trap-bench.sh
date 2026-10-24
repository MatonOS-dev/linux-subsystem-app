#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
iterations=${1:-100000}
repetitions=${2:-5}
mkdir -p build
cc -O2 -Wall -Wextra -Werror tests/trap-bench.c -o build/trap-bench
fixture=$(mktemp -d "$PWD/build/trap-bench.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
printf 'compat proxy trap benchmark fixture\n' >"$fixture/file"

printf 'native and proxied payload syscall cost; each run is a fresh process\n'
for mode in getpid open stat; do
    for route in native proxied; do
        for ((run=1; run<=repetitions; run++)); do
            if [[ $mode == getpid ]]; then path=-
            elif [[ $route == native ]]; then path="$fixture/file"
            else path=/bench/file
            fi
            if [[ $route == native ]]; then
                build/trap-bench "$mode" "$iterations" "$path"
            else
                build/linux-layer-v3 --android-filter --map "$fixture" /bench \
                    build/trap-bench "$mode" "$iterations" "$path"
            fi
        done
    done
done

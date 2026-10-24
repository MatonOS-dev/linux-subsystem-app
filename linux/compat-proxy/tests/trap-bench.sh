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

printf 'native, trapped no-op and proxied payload syscall cost; each run is a fresh process\n'
for mode in noop getpid open stat statx; do
    for route in native proxied rewrite; do
        for ((run=1; run<=repetitions; run++)); do
            if [[ $mode == getpid ]]; then path=-
            elif [[ $route == native ]]; then path="$fixture/file"
            else path=/bench/file
            fi
            printf "route=%s run=%s " "$route" "$run"
            if [[ $route == native ]]; then
                build/trap-bench "$mode" "$iterations" "$path"
            else
                # The no-op uses an unknown syscall that the proxy traps and
                # answers with -ENOSYS. Its per-call diagnostic is deliberately
                # suppressed so stderr writes do not dominate the measurement.
                if [[ $mode == noop ]]; then
                    env MATON_PROXY_REWRITE=$([[ $route == rewrite ]] && echo 1 || echo 0) build/linux-layer-v3 --android-filter --map "$fixture" /bench \
                        build/trap-bench "$mode" "$iterations" "$path" 2>/dev/null
                else
                    env MATON_PROXY_REWRITE=$([[ $route == rewrite ]] && echo 1 || echo 0) build/linux-layer-v3 --android-filter --map "$fixture" /bench \
                        build/trap-bench "$mode" "$iterations" "$path"
                fi
            fi
        done
    done
done

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -O2 -Wall -Wextra -Werror tests/fresh-resident.c -o build/fresh-payload
mkdir -p build/fresh-cwd
printf 'binder marker fixture\n' >build/fresh-binder
chmod 0600 build/fresh-binder
for mode in marked plain; do
    args=()
    [[ $mode == marked ]] && args+=(--map "$PWD/build/fresh-binder" /dev/binderfs/binder)
    timeout 20 build/linux-layer-v3 --trace "${args[@]}" "$PWD/build/fresh-payload" "$mode" \
        >"build/fresh-resident-$mode.out" 2>"build/fresh-resident-$mode.log"
    grep -q 'PASS fresh resident payload state' "build/fresh-resident-$mode.out"
    if [[ $mode == marked ]]; then
        grep -q 'android-state-fresh-exec' "build/fresh-resident-$mode.log"
        grep -q 'exec-state-import' "build/fresh-resident-$mode.log"
    else
        ! grep -q 'android-state-fresh-exec' "build/fresh-resident-$mode.log"
    fi
    cat "build/fresh-resident-$mode.out"
    printf 'PASS %s exec path selection\n' "$mode"
done

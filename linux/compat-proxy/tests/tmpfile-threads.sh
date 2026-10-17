#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -no-pie -O2 -Wall -Wextra -Werror -pthread tests/tmpfile-threads.c -o build/tmpfile-threads
proxy=${TMPFILE_PROXY:-$PWD/build/linux-layer-v3}
for store in absent nested; do
    fixture=$(mktemp -d "$PWD/build/tmpfile-threads.XXXXXX")
    if [[ $store == nested ]]; then
        mkdir -p "$fixture/installation/repo/.v3-inodes"
        chmod 700 "$fixture"/installation{,/repo,/repo/.v3-inodes}
        touch "$fixture/installation/repo/.v3-inodes/lock"
    fi
    timeout 180 build/link-deny "$proxy" --android-filter "$PWD/build/tmpfile-threads" "$fixture" >"build/tmpfile-$store.out" 2>"build/tmpfile-$store.log"
    cat "build/tmpfile-$store.out"
    # No unpublished temp inodes may leak when the threads close their fds.
    test "$(find "$fixture" -name '.matonos-tmp-*' | wc -l)" -eq 0
    rm -r "$fixture"
done

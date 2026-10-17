#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
out=${1:?external output required}
bash "$root/build.sh" "$out"
cc -O2 -Wall -Wextra -Werror "$root/host-test.c" -L"$out" -lmatonos-inprocess -Wl,-rpath,"$out" -o "$out/test"
for mode in static glibc; do
    flags=();[[ $mode != static ]] || flags+=(-static)
    cc -O2 -Wall -Wextra -Werror -pthread "${flags[@]}" "$root/payload.c" -o "$out/$mode-payload"
    timeout 30 "$out/test" "$out/$mode-payload" fixture
 done

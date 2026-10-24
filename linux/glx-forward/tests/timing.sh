#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: timing.sh external-output-directory}
mkdir -p "$out"
for abi in 64 32; do
    "${CC:-cc}" -m"$abi" -O2 -Wall -Wextra -Werror -ffreestanding -nostdlib -static -fno-pie -no-pie -fno-stack-protector "$here/timing.c" -o "$out/timing-$abi"
    timeout 5 "$out/timing-$abi"
    echo "PASS ELF$abi clock_gettime and nanosleep: result, time layout, invalid argument and elapsed time"
done

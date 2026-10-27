#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -O2 -Wall -Wextra -Werror tests/uname-trace.c -o build/uname-trace
build/uname-trace > build/uname-native.out
timeout 15 build/linux-layer-v3 --android-filter --trace "$PWD/build/uname-trace" >build/uname-proxy.out 2>build/uname-proxy.log
cmp build/uname-native.out build/uname-proxy.out
grep -q 'v3 uname.*machine=x86_64' build/uname-proxy.log
grep -q 'v3 uname.*machine=i686' build/uname-proxy.log
grep -q 'v3 uname result=0xfffffffffffffff2' build/uname-proxy.log
echo 'PASS uname diagnostics preserve native personas, machine answers and EFAULT'

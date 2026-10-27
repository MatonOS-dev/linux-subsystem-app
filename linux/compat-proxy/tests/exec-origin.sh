#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/exec-origin
printf 'int origin_fixture(void){return 42;}\n' | cc -x c -shared -fPIC - -o build/exec-origin/liborigin-fixture.so
cc -O2 -Wall -Wextra -Werror tests/exec-origin.c -Lbuild/exec-origin -lorigin-fixture -Wl,-rpath,'$ORIGIN' -o build/exec-origin/payload
payload=/app/payload
timeout 20 build/linux-layer-v3 --android-filter --map "$PWD/build/exec-origin" /app "$payload" >build/exec-origin/direct.out 2>build/exec-origin/direct.log
cat build/exec-origin/direct.out
python3 tests/exec-origin-host.py

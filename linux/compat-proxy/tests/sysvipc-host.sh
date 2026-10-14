#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_cache=$(mktemp -d "${TMPDIR:-/tmp}/v3-ipc-test.XXXXXX")
trap 'rm -rf "$test_cache"' EXIT
export TMPDIR="$test_cache"
for test in workload edge lifecycle info; do
  cc -no-pie -O2 -Wall -Wextra -Werror -pthread "tests/sysvipc-$test.c" -o "build/sysvipc-$test"
  timeout 45 build/linux-layer-v3 --android-filter "$PWD/build/sysvipc-$test"
done
cc -no-pie -O2 -Wall -Wextra -Werror tests/sysvipc-recovery.c raw.S -o build/sysvipc-recovery
timeout 15 build/sysvipc-recovery

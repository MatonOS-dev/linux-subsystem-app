#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -O2 -Wall -Wextra -Werror tests/getifaddrs-route.c -o build/getifaddrs-route
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/getifaddrs-route" >build/getifaddrs-route.out 2>build/getifaddrs-route.log
cat build/getifaddrs-route.out

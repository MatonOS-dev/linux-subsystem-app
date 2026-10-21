#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -DPROXY_LIBRARY -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/resident-fds-driver.c loader.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S -o build/resident-fds-driver
cc -O2 -Wall -Wextra -Werror tests/resident-fds.c -o build/resident-fds-test
timeout 20 build/resident-fds-driver "$PWD/build/resident-fds-test"

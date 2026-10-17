#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -no-pie -O2 -Wall -Wextra -Werror tests/links-incremental.c -o build/links-incremental-test
cc -DCOMPAT_LINKS_TEST -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S -o build/links-test-loader
python3 tests/links-incremental.py

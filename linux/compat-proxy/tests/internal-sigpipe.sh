#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# The driver embeds proxy.c so it exercises the real internal writers rather
# than a copy. It must produce its own PASS: a SIGPIPE death has no output.
cc -DPROXY_LIBRARY -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/internal-sigpipe.c loader.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S -o build/internal-sigpipe
timeout 20 build/internal-sigpipe

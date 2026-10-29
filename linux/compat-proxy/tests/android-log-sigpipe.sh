#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# The driver embeds proxy.c so it exercises the real liblog logger and the real
# payload close_range/dup2 dispatchers, not a copy. It must produce its own
# PASS: a SIGPIPE death has no output.
cc -DPROXY_LIBRARY -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/android-log-sigpipe.c loader.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S -o build/android-log-sigpipe
fixture=$(mktemp -d "$PWD/build/android-log.XXXXXX")
timeout 20 build/android-log-sigpipe "$fixture/logdw"
rm -rf "$fixture"

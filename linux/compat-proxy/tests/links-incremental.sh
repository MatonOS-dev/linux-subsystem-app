#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -no-pie -O2 -Wall -Wextra -Werror tests/links-incremental.c -o build/links-incremental-test
cc -DCOMPAT_LINKS_TEST -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S -o build/links-test-loader
cc -no-pie -O2 -Wall -Wextra -Werror tests/links-pivot.c -o build/links-pivot
python3 tests/links-incremental.py
if command -v bwrap >/dev/null; then
    pivot_fixture=$(mktemp -d "$PWD/build/links-pivot.XXXXXX")
    chmod 700 "$pivot_fixture"
    touch "$pivot_fixture/.v3-store-root"
    timeout 30 build/link-deny "$PWD/build/links-test-loader" --android-filter "$(command -v bwrap)" \
        --ro-bind / / --bind "$pivot_fixture" /tmp --chdir /tmp --proc /proc --dev /dev \
        "$PWD/build/links-pivot" >build/links-pivot.out 2>build/links-pivot.log
    cat build/links-pivot.out
    rm -r "$pivot_fixture"
else
    printf 'SKIP pivot-root handler-stack regression: bwrap unavailable\n'
fi

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${COMPAT_LINKS_TEST_OUT:?external output directory required}"
mkdir -p "$COMPAT_LINKS_TEST_OUT"
out=$(realpath "$COMPAT_LINKS_TEST_OUT")
export COMPAT_LINKS_TEST_OUT=$out
cc -DCOMPAT_LINKS_TEST "-DCOMPAT_LINKS_TEST_ROOT=\"$out\"" -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S -o "$out/linux-layer-v3"
cp "$out/linux-layer-v3" "$out/links-test-loader"
for test in link-deny hardlinks links-incremental; do
 cc -no-pie -O2 -Wall -Wextra -Werror "tests/$test.c" -o "$out/$test$( [[ $test == link-deny ]] || printf -- '-test')"
done
fixture=$(mktemp -d "$out/hardlinks.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
"$out/link-deny" "$out/linux-layer-v3" --android-filter "$out/hardlinks-test" "$fixture"
python3 tests/placeholder-crash.py
python3 tests/links-incremental.py

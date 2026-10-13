#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh
for test in signals threads spawn binder; do
    cc -no-pie -O2 -Wall -Wextra -Werror -pthread "tests/$test.c" -o "build/$test"
    timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/$test" >"build/$test.out" 2>"build/$test.log"
    cat "build/$test.out"
done
timeout 15 build/linux-layer-v3 --android-filter /bin/bash -c 'ls /; cat /etc/os-release' >build/stage1-host.out 2>build/stage1-host.log
cc -no-pie -O2 -Wall -Wextra -Werror tests/mounts.c -o build/mounts
mkdir -p build/fixture-dir
printf 'mount-ok\n' >build/fixture-dir/fixture
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/mounts" "$PWD/build/fixture-dir" >build/mounts.out 2>build/mounts.log
cat build/mounts.out
timeout 15 build/linux-layer-v3 --android-filter /bin/bash -c 'printf pipe-ok | cat' >build/pipeline.out 2>build/pipeline.log
grep -qx pipe-ok build/pipeline.out
printf 'PASS host bash -> ls/cat and dup2 pipeline under copied Android app filter\n'
printf '#!/bin/bash\necho script-ok\n' >build/script
chmod +x build/script
timeout 15 build/linux-layer-v3 --android-filter /bin/bash -c "exec '$PWD/build/script'" >build/script.out 2>build/script.log
grep -qx script-ok build/script.out
printf 'PASS shebang through intercepted exec\n'
cc -nostdlib -static -no-pie -O2 -Wall -Wextra -Werror tests/static.c entry.S -o build/static
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/static" >build/static.out 2>build/static.log
cat build/static.out

mkdir -p build/legacy-fixture build/nested-fixture
ln -sfn /virtual-v3/fixture build/fixture-dir/absolute
ln -sfn fixture build/fixture-dir/relative
for test in legacy paths; do
    cc -no-pie -O2 -Wall -Wextra -Werror "tests/$test.c" -o "build/$test"
done
timeout 20 build/linux-layer-v3 --android-filter "$PWD/build/legacy" "$PWD/build/legacy-fixture" >build/legacy.out 2>build/legacy.log
cat build/legacy.out
timeout 20 build/linux-layer-v3 --android-filter "$PWD/build/paths" "$PWD/build/fixture-dir" "$PWD/build/nested-fixture" >build/paths.out 2>build/paths.log
cat build/paths.out
if command -v bwrap >/dev/null; then
    timeout 20 build/linux-layer-v3 --android-filter "$(command -v bwrap)" --ro-bind / / --tmpfs /tmp --proc /proc --dev /dev /bin/true >build/bwrap.out 2>build/bwrap.log
    timeout 20 build/linux-layer-v3 --android-filter "$(command -v bwrap)" --ro-bind / / --tmpfs /tmp --bind "$PWD/build/fixture-dir" /tmp/fixture --proc /proc --dev /dev /bin/cat /tmp/fixture/fixture >build/bwrap-fixture.out 2>build/bwrap-fixture.log
    grep -qx mount-ok build/bwrap-fixture.out
    printf 'PASS host bwrap virtual root and fixture bind\n'
else
    printf 'SKIP host bwrap unavailable\n'
fi

mkdir -p build/root-scaffold
for name in usr bin lib lib64 etc sbin proc dev; do
    ln -sfn "/$name" "build/root-scaffold/$name"
done
timeout 15 build/linux-layer-v3 --android-filter --root "$PWD/build/root-scaffold" /bin/bash -c 'printf root-pipe-ok | cat' >build/root-scaffold.out 2>build/root-scaffold.log
grep -qx root-pipe-ok build/root-scaffold.out
printf 'PASS --root host-side symlink scaffold\n'

mkdir -p build/process-parent build/process-child
printf 'parent\n' >build/process-parent/fixture
printf 'child\n' >build/process-child/fixture
cc -no-pie -O2 -Wall -Wextra -Werror tests/process.c -o build/process
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/process" "$PWD/build/process-parent" "$PWD/build/process-child" >build/process.out 2>build/process.log
cat build/process.out

cc -no-pie -O2 -Wall -Wextra -Werror tests/net.c -o build/net-test
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/net-test" >build/net-test.out 2>build/net-test.log
cat build/net-test.out

cc -no-pie -O2 -Wall -Wextra -Werror tests/publication.c -o build/publication
mkdir -p build/publication-a build/publication-b
rm -f build/publication-a/{run-environ,empty,new,different}
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/publication" "$PWD/build/publication-a" "$PWD/build/publication-b" >build/publication.out 2>build/publication.log
cat build/publication.out

cc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections tests/dns-translation.c -o build/dns-translation
build/dns-translation
# Real loopback tests require AF_INET socket permission: python3 tests/dns-host.py

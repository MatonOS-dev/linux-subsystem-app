#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh
cc -O2 -Wall -Wextra -Werror tests/arch-map.c -o build/arch-map
build/arch-map
if [[ ${1:-} == --i386-p2 ]]; then
    bash tests/i386-p1.sh
    exec bash tests/i386-dynamic.sh
fi
if [[ ${1:-} == --i386-p1 ]]; then
    exec bash tests/i386-p1.sh
fi
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

cc -no-pie -O2 -Wall -Wextra -Werror tests/link-deny.c -o build/link-deny
cc -no-pie -O2 -Wall -Wextra -Werror tests/publication.c -o build/publication
mkdir -p build/publication-a build/publication-b
chmod 700 build/publication-a build/publication-b
rm -f build/publication-a/{run-environ,empty,new,different}
timeout 15 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/publication" "$PWD/build/publication-a" "$PWD/build/publication-b" >build/publication.out 2>build/publication.log
cat build/publication.out

cc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections tests/dns-translation.c -o build/dns-translation
build/dns-translation
# Real Unix tests run when the host permits socket bind.
python3 tests/dns-host.py --skip-unavailable

cc -no-pie -O2 -Wall -Wextra -Werror tests/links.c -o build/links-test
links_fixture=$(mktemp -d "$PWD/build/links-fixture.XXXXXX")
timeout 20 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/links-test" "$links_fixture" >build/links-test.out 2>build/links-test.log
cat build/links-test.out
rm -r "$links_fixture"

cc -no-pie -O2 -Wall -Wextra -Werror tests/hardlinks.c -o build/hardlinks-test
hardlinks_fixture=$(mktemp -d "$PWD/build/hardlinks-fixture.XXXXXX")
timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$hardlinks_fixture" >build/hardlinks-test.out 2>build/hardlinks-test.log
cat build/hardlinks-test.out
# Backing objects and count sidecars disappear after the final name.
test "$(find "$hardlinks_fixture/.v3-inodes" -type f ! -name lock | wc -l)" -eq 0
rm -r "$hardlinks_fixture"

crash_fixture=$(mktemp -d "$PWD/build/crash-fixture.XXXXXX")
python3 - "$crash_fixture" <<'PYCRASH'
import pathlib, sys
root=pathlib.Path(sys.argv[1]).resolve()
store=root/'.v3-inodes'; store.mkdir(mode=0o700)
(store/'i-orphan').write_text('orphan')
(store/'i-missing.count').write_bytes(b'\0'*8)
(store/'i-restored').write_text('interrupted conversion')
(store/'i-survivor').write_text('live')
(root/'survivor').symlink_to(store/'i-survivor')
(store/'conversion').write_bytes(str(root/'restored').encode()+b'\0'+str(store/'i-restored').encode()+b'\0')
(store/'dirty').touch()
PYCRASH
timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$crash_fixture" --recover >build/crash-test.out 2>build/crash-test.log
cat build/crash-test.out
test ! -e "$crash_fixture/.v3-inodes/i-orphan"
test ! -e "$crash_fixture/.v3-inodes/i-missing.count"
test ! -e "$crash_fixture/.v3-inodes/i-restored"
test ! -e "$crash_fixture/.v3-inodes/conversion"
rm -r "$crash_fixture"

independent_fixture=$(mktemp -d "$PWD/build/independent-fixture.XXXXXX")
timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$independent_fixture" --prepare >build/independent-test.out 2>build/independent-test.log
independent_pids=()
for i in {1..8}; do
    timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$independent_fixture" --link "independent-$i" >>build/independent-test.out 2>>build/independent-test.log &
    independent_pids+=("$!")
done
for pid in "${independent_pids[@]}"; do wait "$pid"; done
timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$independent_fixture" --check >>build/independent-test.out 2>>build/independent-test.log
cat build/independent-test.out
rm -r "$independent_fixture"

move_fixture=$(mktemp -d "$PWD/build/move-fixture.XXXXXX")
python3 - "$move_fixture" <<'PYMOVE'
import pathlib, sys
root=pathlib.Path(sys.argv[1]).resolve()
old=root.with_name(root.name+'.old')
store=root/'.v3-inodes'; store.mkdir(mode=0o700)
(root/'restored').write_text('ordinary file')
(store/'i-survivor').write_text('moved inode')
(root/'survivor').symlink_to(old/'.v3-inodes/i-survivor')
(store/'move').write_bytes(str(old).encode()+b'\0'+str(root).encode()+b'\0')
(store/'dirty').touch()
PYMOVE
timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$move_fixture" --recover >build/move-test.out 2>build/move-test.log
test ! -e "$move_fixture/.v3-inodes/move"
printf 'PASS interrupted store-root rename recovery\n'
rm -r "$move_fixture"

COMPAT_GPU_SKIP_BUILD=1 bash tests/gpu-host.sh

cc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections tests/temp-gc.c -o build/temp-gc
temp_fixture=$(mktemp -d "$PWD/build/temp-gc.XXXXXX")
build/temp-gc "$temp_fixture"
rm -r "$temp_fixture"

python3 tests/pty-host.py

python3 tests/host-command-pair.py
bash tests/sysvipc-host.sh

python3 tests/host-command-threads.py

bash tests/i386-p1.sh
bash tests/i386-dynamic.sh

bash tests/cross-exec.sh

MATON_V3_ANDROID_OUT="$PWD/build/android-test" bash build-android.sh
python3 tests/packaging.py

bash tests/exec-errors.sh

python3 tests/pack-host.py

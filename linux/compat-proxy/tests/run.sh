#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh
cc -O2 -Wall -Wextra -Werror tests/payload-filter.c -o build/payload-filter
build/payload-filter
cc -O2 -Wall -Wextra -Werror -pthread tests/i386-thread-adopt.c \
    arch/i386/thread-adopt.c -o build/i386-thread-adopt
timeout 15 build/i386-thread-adopt
cc -O2 -Wall -Wextra -Werror tests/arch-map.c -o build/arch-map
build/arch-map
cc -O2 -Wall -Wextra -Werror tests/resident-ranges.c -o build/resident-ranges
build/resident-ranges
bash tests/resident-fds.sh
cc -DPROXY_LIBRARY -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/resident-altstack.c loader.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S -o build/resident-altstack
build/resident-altstack
cc -O2 -Wall -Wextra -Werror -pthread tests/raw-clone-gateway.c tests/raw-clone-gateway.S -o build/raw-clone-gateway
timeout 10 build/raw-clone-gateway
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
cc -no-pie -O2 -Wall -Wextra -Werror tests/fcntl-native.c -o build/fcntl-native
timeout 20 build/linux-layer-v3 --android-filter "$PWD/build/fcntl-native" >build/fcntl-native.out 2>build/fcntl-native.log
cat build/fcntl-native.out
grep -q 'v3 trap ours-exec' build/fcntl-native.log
printf 'PASS proxy-private diagnostic fd remains usable after resident exec\n'
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
rm -f build/fixture-dir/hard-symlink
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

cc -no-pie -O2 -Wall -Wextra -Werror tests/oldroot-source.c -o build/oldroot-source
cp build/oldroot-source build/root-scaffold/oldroot-source
timeout 15 build/linux-layer-v3 --android-filter --root "$PWD/build/root-scaffold" /oldroot-source >build/oldroot-source.out 2>build/oldroot-source.log
cat build/oldroot-source.out

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
cc -O2 -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections tests/dns-helper.c -o build/dns-helper
timeout 5 build/dns-helper
cc -no-pie -O2 -Wall -Wextra -Werror tests/links.c -o build/links-test
links_fixture=$(mktemp -d "$PWD/build/links-fixture.XXXXXX")
timeout 20 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/links-test" "$links_fixture" >build/links-test.out 2>build/links-test.log
cat build/links-test.out
rm -r "$links_fixture"

# Keep each private fixture its own legacy boundary, including when build/
# is a symlink to an external output tree with private ancestors.
chmod g+w build
cc -no-pie -O2 -Wall -Wextra -Werror tests/hardlinks.c -o build/hardlinks-test
marker_fixture=$(mktemp -d "$PWD/build/marker-fixture.XXXXXX")
python3 - "$marker_fixture" <<'PYMARKERS'
import os, pathlib, sys
root=pathlib.Path(sys.argv[1]).resolve()
for name in ('outer/deep', 'outer/nested/deep', 'sibling', 'plain/deep', 'symlink/deep', 'foreign/deep'):
    (root/name).mkdir(parents=True, mode=0o700)
for directory in root.rglob('*'):
    if directory.is_dir(): directory.chmod(0o700)
for name in ('outer', 'outer/nested', 'sibling'):
    (root/name/'.v3-store-root').touch(mode=0o600)
(root/'symlink/.v3-store-root').symlink_to(root/'outer/.v3-store-root')
foreign=root/'foreign/.v3-store-root'
foreign.touch(mode=0o600)
if os.geteuid()==0:
    os.chown(foreign, 65534, -1)
else:
    foreign.unlink()
    foreign.symlink_to('/etc/passwd')
    print('SKIP foreign-owned regular marker fixture requires root; symlink covered')
PYMARKERS
cc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections tests/store-marker.c -o build/store-marker
build/store-marker "$marker_fixture/outer"
timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$marker_fixture" --markers >build/markers.out 2>build/markers.log
cat build/markers.out
# Inspect stores outside the proxy; unmarked roots must stay untouched.
python3 - "$marker_fixture" <<'PYMARKERS'
import pathlib, sys, shutil
root=pathlib.Path(sys.argv[1]).resolve()
for name, boundary in [('outer/deep','outer'), ('outer/nested/deep','outer/nested')]:
    p=root/name/'a'; st=p.stat()
    assert p.is_file() and not p.is_symlink() and st.st_size==0 and st.st_mode&0o1000
    entry=root/boundary/'.v3-inodes'/f'p-{st.st_dev:016x}-{st.st_ino:016x}'
    assert (entry.parent/entry.read_bytes().rstrip(b'\0').decode()).is_file()
for name in ('plain','symlink','foreign'):
    assert not (root/name/'deep/b').exists()
assert not (root/'.v3-inodes').exists()
assert not (root/'outer/deep/.v3-inodes').exists()
shutil.rmtree(root)
PYMARKERS
printf 'PASS nearest/nested markers and no unmarked stores\n'
hardlinks_fixture=$(mktemp -d "$PWD/build/hardlinks-fixture.XXXXXX")
touch "$hardlinks_fixture/.v3-store-root"
timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$hardlinks_fixture" >build/hardlinks-test.out 2>build/hardlinks-test.log
if [[ -n ${MATON_TEST_GLNX_SOURCE:-} ]]; then bash tests/glnx-nofollow.sh; fi
cat build/hardlinks-test.out
printf 'PASS no-follow stored-name and real-symlink regression checks\n'
# Backing objects and count sidecars disappear after the final name.
test "$(find "$hardlinks_fixture/.v3-inodes" -type f ! -name lock | wc -l)" -eq 0
rm -r "$hardlinks_fixture"

crash_fixture=$(mktemp -d "$PWD/build/crash-fixture.XXXXXX")
touch "$crash_fixture/.v3-store-root"
python3 - "$crash_fixture" <<'PYCRASH'
import pathlib, sys
root=pathlib.Path(sys.argv[1]).resolve()
store=root/'.v3-inodes'; store.mkdir(mode=0o700)
(store/'i-orphan').write_text('orphan')
(store/'i-missing.count').write_bytes(b'\0'*8)
(store/'i-restored').write_text('interrupted conversion')
obj='i-'+'a'*32
(store/obj).write_text('live')
p=root/'survivor';p.touch();p.chmod(0o1600);st=p.stat()
(store/f'p-{st.st_dev:016x}-{st.st_ino:016x}').write_bytes(obj.encode()+b'\0')
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
touch "$independent_fixture/.v3-store-root"
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

bash tests/placeholder-raw.sh
python3 tests/placeholder-crash.py
bash tests/ostree-links.sh
bash tests/links-incremental.sh
# Mixed-class exec requires a non-writable peer-loader directory.
chmod g-w build

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
python3 tests/icd-dup-host.py

python3 tests/loader-maps.py

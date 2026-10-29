#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh
python3 tests/mount-scan.py
python3 tests/marker-cache.py
native_out=$(mktemp -d "${TMPDIR:-/mnt/data/aosp/out/pc-logs}/steam-personality-native.XXXXXX")
bash tests/native-reservation.sh "$native_out/guards" >"$native_out/guards.log" 2>&1
grep '^PASS ' "$native_out/guards.log"
bash tests/bionic32-start.sh "$native_out/bionic32"
bash tests/home-links.sh
bash tests/exec-origin.sh
cc -O2 -Wall -Wextra -Werror tests/reopen-dirfd.c -o build/reopen-dirfd
reopen_fixture=$(mktemp -d "$PWD/build/reopen-dirfd.XXXXXX")
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/reopen-dirfd" "$reopen_fixture/dir" >build/reopen-dirfd.out 2>build/reopen-dirfd.log
timeout 15 build/linux-layer-v3 --android-filter --map "$reopen_fixture" /reopened-dirfd "$PWD/build/reopen-dirfd" /reopened-dirfd/dir >build/reopen-dirfd-map.out 2>build/reopen-dirfd-map.log
rmdir "$reopen_fixture"
cat build/reopen-dirfd.out build/reopen-dirfd-map.out
bash tests/uname-trace.sh
cc -O2 -Wall -Wextra -Werror tests/proc-dirfd.c -o build/proc-dirfd
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/proc-dirfd" >build/proc-dirfd.out 2>build/proc-dirfd.log
cat build/proc-dirfd.out
cc -O2 -Wall -Wextra -Werror tests/inotify-read.c -o build/inotify-read-test
build/inotify-read-test
cc -O2 -Wall -Wextra -Werror tests/create-denied.c -o build/create-denied
mkdir -p build/create-denied-dir
chmod 0500 build/create-denied-dir
timeout 15 build/linux-layer-v3 --android-filter --map "$PWD/build/create-denied-dir" /denied "$PWD/build/create-denied" >build/create-denied.out 2>build/create-denied.log
chmod 0700 build/create-denied-dir
grep -q 'v3 create input /denied/tmp.fixture' build/create-denied.log
grep -q "v3 create backing $PWD/build/create-denied-dir/tmp.fixture" build/create-denied.log
cat build/create-denied.out
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
for test in signals threads spawn binder zygote-startup sigsys-virtual; do
    cc -no-pie -O2 -Wall -Wextra -Werror -pthread "tests/$test.c" -o "build/$test"
    if [[ $test == sigsys-virtual ]]; then timeout 15 "build/$test"; fi
    # 10,000 thread creations can exceed 15 s on the shared build host.
    test_timeout=15
    if [[ $test == threads ]]; then test_timeout=60; fi
    timeout "$test_timeout" build/linux-layer-v3 --android-filter "$PWD/build/$test" >"build/$test.out" 2>"build/$test.log"
    cat "build/$test.out"
done
cc -O2 -Wall -Wextra -Werror tests/prehandler-filter.c -o build/prehandler-filter
cc -O2 -Wall -Wextra -Werror tests/supervise-child.c -o build/supervise-child
timeout 15 build/prehandler-filter build/linux-layer-v3 --supervise --android-filter "$PWD/build/supervise-child" >build/prehandler-filter.out 2>build/prehandler-filter.log
printf 'PASS loader supervisor starts under the generated Android app syscall allowlist\n'
python3 tests/supervise.py build/linux-layer-v3 build/linux-layer-v3-i386
bash ../../tests/gui-process.sh "${TMPDIR:-/mnt/data/aosp/out/pc-logs}/steam-personality-gui-process" "$PWD/build/linux-layer-v3" --supervise --android-filter
python3 tests/stderr-trace.py
cc -no-pie -O2 -Wall -Wextra -Werror tests/fcntl-native.c -o build/fcntl-native
timeout 20 build/linux-layer-v3 --android-filter "$PWD/build/fcntl-native" >build/fcntl-native.out 2>build/fcntl-native.log
cat build/fcntl-native.out
grep -q 'v3 trap ours-exec' build/fcntl-native.log
printf 'PASS proxy-private diagnostic fd remains usable after resident exec\n'
cc -no-pie -O2 -Wall -Wextra -Werror tests/fd-reserve.c -o build/fd-reserve
timeout 20 build/linux-layer-v3 --android-filter "$PWD/build/fd-reserve" >build/fd-reserve.out 2>build/fd-reserve.log
grep -q 'PASS private fd pool survives' build/fd-reserve.out
timeout 15 build/linux-layer-v3 --android-filter /bin/bash -c 'ls /; cat /etc/os-release' >build/stage1-host.out 2>build/stage1-host.log
cc -no-pie -O2 -Wall -Wextra -Werror tests/mounts.c -o build/mounts
mkdir -p build/fixture-dir
printf 'mount-ok\n' >build/fixture-dir/fixture
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/mounts" "$PWD/build/fixture-dir" >build/mounts.out 2>build/mounts.log
cat build/mounts.out
cc -no-pie -O2 -Wall -Wextra -Werror tests/resolve-cache.c -o build/resolve-cache
mkdir -p build/cache-source-a build/cache-source-b
printf a >build/cache-source-a/file
printf bb >build/cache-source-b/file
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/resolve-cache" \
    "$PWD/build/cache-source-a" "$PWD/build/cache-source-b" >build/resolve-cache.out 2>build/resolve-cache.log
cat build/resolve-cache.out
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
ln -sfn /virtual-v3/fixture build/nested-fixture/cross-mount
for test in legacy paths; do
    cc -no-pie -O2 -Wall -Wextra -Werror "tests/$test.c" -o "build/$test"
done
timeout 20 build/linux-layer-v3 --android-filter "$PWD/build/legacy" "$PWD/build/legacy-fixture" >build/legacy.out 2>build/legacy.log
cat build/legacy.out
timeout 20 build/linux-layer-v3 --android-filter "$PWD/build/paths" "$PWD/build/fixture-dir" "$PWD/build/nested-fixture" >build/paths.out 2>build/paths.log
cat build/paths.out
cc -no-pie -O2 -Wall -Wextra -Werror tests/posix-shm.c -o build/posix-shm
timeout 20 build/linux-layer-v3 --android-filter "$PWD/build/posix-shm" >build/posix-shm-direct.out 2>build/posix-shm-direct.log
cat build/posix-shm-direct.out
if command -v bwrap >/dev/null; then
    timeout 20 build/linux-layer-v3 --android-filter "$(command -v bwrap)" --ro-bind / / --tmpfs /tmp --proc /proc --dev /dev /bin/true >build/bwrap.out 2>build/bwrap.log
    timeout 20 build/linux-layer-v3 --android-filter "$(command -v bwrap)" --ro-bind / / --tmpfs /tmp --bind "$PWD/build/fixture-dir" /tmp/fixture --proc /proc --dev /dev /bin/cat /tmp/fixture/fixture >build/bwrap-fixture.out 2>build/bwrap-fixture.log
    grep -qx mount-ok build/bwrap-fixture.out
    timeout 20 build/linux-layer-v3 --android-filter "$(command -v bwrap)" \
        --ro-bind / / --proc /proc --dev /dev \
        "$PWD/build/posix-shm" >build/posix-shm.out 2>build/posix-shm.log
    cat build/posix-shm.out
    printf 'PASS host bwrap virtual root and fixture bind\n'
else
    printf 'SKIP host bwrap unavailable\n'
fi
python3 tests/mkdir-home-host.py

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

cc -no-pie -O2 -Wall -Wextra -Werror tests/bwrap-filter-name.c -o build/bwrap-filter-name
cp build/bwrap-filter-name build/libmatonos-app-bwrap.so
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/bwrap-filter-name" launch "$PWD/build/libmatonos-app-bwrap.so" >build/bwrap-filter-name.out 2>build/bwrap-filter-name.log
cat build/bwrap-filter-name.out
cc -no-pie -O2 -Wall -Wextra -Werror tests/flatpak-seccomp.c -o build/flatpak-seccomp
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/flatpak-seccomp" >build/flatpak-seccomp.out 2>build/flatpak-seccomp.log
cat build/flatpak-seccomp.out
cc -no-pie -O2 -Wall -Wextra -Werror tests/flatpak-seccomp-fork.c -o build/flatpak-seccomp-fork
bash -c 'ulimit -n 32768; exec timeout 15 "$@"' _ \
    build/linux-layer-v3 --android-filter "$PWD/build/flatpak-seccomp-fork" \
    >build/flatpak-seccomp-fork.out 2>build/flatpak-seccomp-fork.log
cat build/flatpak-seccomp-fork.out
cc -O2 -Wall -Wextra -Werror tests/adopt-altstack-host.c arch/x86_64/altstack.S -o build/adopt-altstack-host
timeout 15 build/adopt-altstack-host
cc -DPROXY_LIBRARY -nostdlib -static -no-pie -mcmodel=large -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/adopt-altstack-filter.c loader.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S arch/x86_64/altstack.S -o build/adopt-altstack-filter
timeout 15 build/adopt-altstack-filter
cc -DPROXY_LIBRARY -fPIE -pie -mcmodel=large -Wl,--no-relax -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -Wl,-z,now -Wl,--export-dynamic-symbol=pthread_create -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/adopt-rebase-filter.c loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c adopt-map.c arch/x86_64/raw.S arch/x86_64/adopt.S arch/x86_64/altstack.S -pthread -ldl -o build/adopt-rebase-filter
timeout 15 build/adopt-rebase-filter
printf 'PASS resident adoption rebase inside SIGSYS and outside altstack, main and fork child\n'
cc -DPROXY_LIBRARY -fPIE -pie -mcmodel=large -Wl,--no-relax -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -Wl,-z,now -Wl,--export-dynamic-symbol=pthread_create -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/adopt-fork-launch.c loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c adopt-map.c arch/x86_64/raw.S arch/x86_64/adopt.S arch/x86_64/altstack.S -pthread -ldl -o build/adopt-fork-launch
cc -O2 -Wall -Wextra -Werror -pthread tests/exec-signalfd.c -o build/exec-signalfd
build/exec-signalfd
for mode in monitor nonleader; do
    timeout 20 build/adopt-fork-launch "$PWD/build/exec-signalfd" "$mode" >"build/exec-signalfd-$mode.out" 2>"build/exec-signalfd-$mode.log"
    cat "build/exec-signalfd-$mode.out"
done
# No argument creates the pre-exec sibling in the group leader.
timeout 20 build/adopt-fork-launch "$PWD/build/exec-signalfd" >build/exec-signalfd-adopt.out 2>build/exec-signalfd-adopt.log
cat build/exec-signalfd-adopt.out
for mode in sigwait handler readv secondary ignore nocldwait; do
    timeout 20 build/exec-signalfd "launch-$mode"
    timeout 20 build/adopt-fork-launch "$PWD/build/exec-signalfd" "launch-$mode" >"build/exec-signalfd-$mode.out" 2>"build/exec-signalfd-$mode.log"
    cat "build/exec-signalfd-$mode.out"
done
cc -O2 -Wall -Wextra -Werror tests/signalfd-child.c -o build/signalfd-child
build/signalfd-child
timeout 15 build/linux-layer-v3 --trace --android-filter "$PWD/build/signalfd-child" >build/signalfd-child.out 2>build/signalfd-child.log
cat build/signalfd-child.out
timeout 15 build/adopt-fork-launch --trace "$PWD/build/signalfd-child" >build/signalfd-adopt.out 2>build/signalfd-adopt.log
cat build/signalfd-adopt.out
cc -DPROXY_LIBRARY -fPIE -pie -mcmodel=large -Wl,--no-relax -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -Wl,-z,now -Wl,--export-dynamic-symbol=pthread_create -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/resident-signalfd-launch.c loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c adopt-map.c arch/x86_64/raw.S arch/x86_64/adopt.S arch/x86_64/altstack.S -pthread -ldl -o build/resident-signalfd-launch
timeout 15 build/linux-layer-v3 --supervise --android-filter "$PWD/build/signalfd-child" >build/signalfd-supervised.out 2>build/signalfd-supervised.log
cat build/signalfd-supervised.out
timeout 15 build/resident-signalfd-launch "$PWD/build/signalfd-child" >build/signalfd-resident.out 2>build/signalfd-resident.log
cat build/signalfd-resident.out
# Exercise upstream Flatpak and bwrap, with the supervisor and resident
# pthread present. Runtime-only command avoids launching an unrelated GUI app.
if command -v flatpak >/dev/null && flatpak info org.freedesktop.Platform//26.08 >/dev/null 2>&1; then
    ln -sf "$(command -v flatpak)" build/libmatonos-app-flatpak.so
    for fork_mode in normal inherited; do
        fork_args=()
        if [[ $fork_mode == inherited ]]; then fork_args=(--raw-fork); fi
        timeout 30 build/resident-signalfd-launch "${fork_args[@]}" "$PWD/build/libmatonos-app-flatpak.so" run \
            --nosocket=x11 --nosocket=wayland --nosocket=session-bus --nosocket=system-bus \
            --no-a11y-bus --no-documents-portal --command=true org.freedesktop.Platform//26.08 \
            >"build/flatpak-resident-$fork_mode.out" 2>"build/flatpak-resident-$fork_mode.log"
    done
    echo "PASS real app-flatpak -> flatpak run -> bwrap -> true with resident workers"
else
    echo "SKIP real Flatpak launch: installed FreeDesktop 26.08 runtime required"
fi
cc -O2 -Wall -Wextra -Werror tests/exec-wayland.c -pthread $(pkg-config --cflags --libs wayland-client) -o build/exec-wayland
cc -DPROXY_LIBRARY -fPIE -pie -mcmodel=large -Wl,--no-relax -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 -Wl,-z,now -Wl,--export-dynamic-symbol=pthread_create -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror tests/resident-wayland-launch.c loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c adopt-map.c arch/x86_64/raw.S arch/x86_64/adopt.S arch/x86_64/altstack.S -pthread -ldl $(pkg-config --cflags --libs wayland-server) -o build/resident-wayland-launch
timeout 20 build/resident-wayland-launch "$PWD/build/exec-wayland" >build/exec-wayland.out 2>build/exec-wayland.log
echo "PASS threaded resident exec connects and completes Wayland roundtrip"
timeout 20 build/adopt-fork-launch "$PWD/build/flatpak-seccomp-fork"
timeout 20 build/adopt-fork-launch "$PWD/build/sigsys-virtual"
timeout 20 build/adopt-fork-launch --trace "$PWD/build/sigsys-virtual" >build/sigsys-adopt-trace.out 2>build/sigsys-adopt-trace.log
cat build/sigsys-adopt-trace.out
timeout 20 build/linux-layer-v3 --trace --android-filter "$PWD/build/sigsys-virtual" >build/sigsys-trace.out 2>build/sigsys-trace.log
cat build/sigsys-trace.out
cc -no-pie -O2 -Wall -Wextra -Werror tests/personality.c -o build/personality
timeout 15 build/linux-layer-v3 --android-filter "$PWD/build/personality" >build/personality.out 2>build/personality.log
cat build/personality.out

cc -no-pie -O2 -Wall -Wextra -Werror tests/passwd-map.c -o build/passwd-map
python3 tests/passwd-map-host.py "$PWD/build/linux-layer-v3" "$PWD/build/passwd-map"

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

# A valid i386 mmap result above 2 GiB is a pointer, not a negative errno.
cc -m32 -nostdlib -static -no-pie tests/i386-dns-start.S -o build/i386-dns-start
timeout 15 build/linux-layer-v3-i386 --android-filter --dns-forwarder 127.0.0.53:53 \
    "$PWD/build/i386-dns-start" >build/i386-dns-start.out 2>build/i386-dns-start.log
printf 'PASS i386 DNS helper startup with high-address stack mapping\n'
# The helper thread must enter dns_worker on an ABI-aligned stack. Its query
# path is the first place -msse2 codegen reaches an aligned frame slot, and a
# 4-byte slip used to kill the payload with an unreported SIGSEGV.
cc -m32 -nostdlib -static -no-pie tests/i386-dns-query.S -o build/i386-dns-query
timeout 15 build/linux-layer-v3-i386 --android-filter --dns-forwarder 127.0.0.53:53 \
    "$PWD/build/i386-dns-query" >build/i386-dns-query.out 2>build/i386-dns-query.log
cat build/i386-dns-query.out
printf 'PASS i386 DNS helper query on the ABI-aligned helper stack\n'
cc -O2 -Wall -Wextra -Werror -Wno-array-bounds -Wno-stringop-overflow -ffunction-sections -fdata-sections -Wl,--gc-sections tests/dns-translation.c -o build/dns-translation
build/dns-translation
cc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections tests/dns-host.c -o build/dns-host
build/dns-host
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
MATON_HC_RESIDENT=1 python3 tests/host-command-pair.py
bash tests/sysvipc-host.sh

python3 tests/host-command-threads.py

bash tests/i386-p1.sh
bash tests/i386-dynamic.sh

bash tests/cross-exec.sh

MATON_V3_ANDROID_OUT="$PWD/build/android-test" bash build-android.sh
python3 tests/packaging.py

bash tests/exec-errors.sh

python3 tests/erofs-host.py
python3 tests/erofs-mkdir-host.py
python3 tests/erofs-single-backing-host.py
python3 tests/erofs-ldconfig-host.py
python3 tests/icd-dup-host.py
bash tests/musl-host.sh
logical_bind_fixture=$(mktemp -d "$PWD/build/logical-bind.XXXXXX")
touch "$logical_bind_fixture/.v3-store-root"
timeout 30 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$logical_bind_fixture" --bind "$PWD/build/musl-host-root/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/bwrap" >build/logical-bind.out 2>build/logical-bind.log
cat build/logical-bind.out
rm -r "$logical_bind_fixture"
chrome_out=${MATON_CHROME_TEST_OUT:-/mnt/data/aosp/out/pc-logs/chrome-sol-host-launch}
mkdir -p "$chrome_out/classes" "$chrome_out/runtime"
javac -d "$chrome_out/classes" \
    "$PWD/../../linux/subsystem/host/app/src/main/java/org/matonos/linux/FlatpakPerAppDirs.java" \
    "$PWD/../../tests/chrome-sol/FlatpakPerAppDirsTest.java"
java -cp "$chrome_out/classes" FlatpakPerAppDirsTest "$chrome_out/runtime"
python3 "$PWD/../../tests/chrome-sol/per-app-dirs-ref-host.py" "$chrome_out/runtime" "$chrome_out"
bash tests/apply-extra-host.sh "${MATON_APPLY_EXTRA_TEST_OUT:-/mnt/data/aosp/out/pc-logs/chrome-sol-host}"

python3 tests/loader-maps.py

bash tests/site-rewrite.sh

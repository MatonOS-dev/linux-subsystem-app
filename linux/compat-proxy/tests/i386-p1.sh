#!/usr/bin/env bash
# ELF32 static VFS regression under the copied x86 app policy.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/i386-p1
cc -m32 -nostdlib -static -no-pie -Wl,--build-id=none \
    tests/i386-static.S -o build/i386-p1/hello
printf 'i386-p1-ok\n!!' > build/i386-p1/fixture
cc -m32 -nostdlib -static -no-pie -Wl,--build-id=none -DI386_EXIT_ONLY \
    tests/i386-static.S -o build/i386-p1/exit-only
set +e
timeout 10 build/i386-p1/exit-only
compat_result=$?
set -e
if (( compat_result != 0 )); then
    printf 'FAIL P1 prerequisite: ELF32 exit-only exited %s; check inherited runner seccomp/IA32 support.\n' "$compat_result" >&2
    exit 1
fi
# Prove the fixture and this host's compat execution independently of the proxy.
set +e
timeout 10 build/i386-p1/hello "$PWD/build/i386-p1/fixture" > build/i386-p1/native.out
native_result=$?
set -e
if (( native_result != 0 )); then
    printf 'FAIL P1 prerequisite: native ELF32 exited %s; check inherited runner seccomp/IA32 support.\n' "$native_result" >&2
    exit 1
fi
cmp build/i386-p1/fixture build/i386-p1/native.out
loader=${MATON_V3_I386_LOADER:-$PWD/build/linux-layer-v3-i386}
set +e
timeout 10 "$loader" --android-filter \
    --map "$PWD/build/i386-p1/fixture" /i386-p1-fixture \
    "$PWD/build/i386-p1/hello" /i386-p1-fixture \
    >build/i386-p1/proxy.out 2>build/i386-p1/proxy.log
result=$?
set -e
if (( result != 0 )); then
    cat build/i386-p1/proxy.log >&2
    printf 'FAIL P1: ELF32 through proxy exited %s (native fixture passed).\n' "$result" >&2
    exit 1
fi
cmp build/i386-p1/fixture build/i386-p1/proxy.out
printf 'PASS P1: i386 stat64/open/read/write with VFS map and copied app policy\n'

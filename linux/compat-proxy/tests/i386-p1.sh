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
cc -m32 -nostdlib -static -no-pie -Wl,--build-id=none \
    tests/i386-tls-raw.S -o build/i386-p1/tls-raw
timeout 10 "$loader" --android-filter "$PWD/build/i386-p1/tls-raw" \
    >build/i386-p1/tls-raw.out 2>build/i386-p1/tls-raw.log
printf 'PASS i386 raw TLS set/get and GS selector read under proxy\n'
cc -m32 -nostdinc -fno-stack-protector -fno-pic -O2 -Wall -Wextra -Werror \
    -c tests/i386-tls.c -o build/i386-p1/tls.o
libc=${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}
interpreter=${MATON_V3_I386_INTERPRETER:-/lib/ld-linux.so.2}
ld -m elf_i386 -o build/i386-p1/tls --dynamic-linker "$interpreter" \
    build/i386-p1/entry.o build/i386-p1/tls.o "$libc"
timeout 20 "$loader" --android-filter --i386-tls-reserve "$PWD/build/i386-p1/tls" \
    >build/i386-p1/tls.out 2>build/i386-p1/tls.log
grep -qx 'PASS i386 pthread TLS, set/get_thread_area, GS read, modify_ldt, LDT FS remap, and SIGSEGV chaining' build/i386-p1/tls.out
cat build/i386-p1/tls.out

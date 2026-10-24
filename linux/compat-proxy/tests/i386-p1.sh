#!/usr/bin/env bash
# ELF32 static VFS regression under the copied x86 app policy.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/i386-p1
libc=${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}
interpreter=${MATON_V3_I386_INTERPRETER:-/lib/ld-linux.so.2}
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
cc -m32 -c tests/i386-entry.S -o build/i386-p1/entry.o
libc=${MATON_V3_I386_LIBC:-/usr/lib32/libc.so.6}
interpreter=${MATON_V3_I386_INTERPRETER:-/lib/ld-linux.so.2}
ld -m elf_i386 -o build/i386-p1/tls --dynamic-linker "$interpreter" \
    build/i386-p1/entry.o build/i386-p1/tls.o "$libc"
timeout 20 "$loader" --android-filter "$PWD/build/i386-p1/tls" \
    >build/i386-p1/tls.out 2>build/i386-p1/tls.log
grep -qx 'PASS i386 pthread TLS, flat LDT code, far transfers, segment loads, and SIGSEGV chaining' build/i386-p1/tls.out
cat build/i386-p1/tls.out

# AT_SYSINFO is on by default; MATON_PROXY_REWRITE=0 exercises the trap route.
# Exercise the exact %gs:0x10 entry with ELF32 glibc,
# including file-class dispatch, asynchronous signals and copied TLS on fork/
# pthread/exec. Both modes must preserve the same syscall results.
cc -m32 -nostdinc -fno-stack-protector -fno-pic -O2 -Wall -Wextra -Werror \
    -c tests/i386-sysinfo.c -o build/i386-p1/sysinfo.o
cc -m32 -c tests/i386-sysinfo.S -o build/i386-p1/sysinfo-entry.o
cc -m32 -c tests/i386-entry.S -o build/i386-p1/sysinfo-start.o
ld -m elf_i386 -o build/i386-p1/sysinfo --dynamic-linker "$interpreter" \
    build/i386-p1/sysinfo-start.o build/i386-p1/sysinfo-entry.o \
    build/i386-p1/sysinfo.o "$libc"
ln -sf fixture "$PWD/build/i386-p1/fastlink"
for rewrite in default 0; do
    if [[ $rewrite == default ]]; then
        env_prefix=(env -u MATON_PROXY_REWRITE)
        label=default
    else
        env_prefix=(env MATON_PROXY_REWRITE=0)
        label=0
    fi
    set +e
    "${env_prefix[@]}" timeout 45 "$loader" --android-filter \
        --map "$PWD/build/i386-p1/fixture" /i386-fastpath-file \
        --map "$PWD/build/i386-p1/fastlink" /i386-fastpath-link \
        "$PWD/build/i386-p1/sysinfo" >"build/i386-p1/sysinfo-$label.out" \
        2>"build/i386-p1/sysinfo-$label.log"
    result=$?
    set -e
    if (( result != 0 )); then cat "build/i386-p1/sysinfo-$label.log" >&2; exit "$result"; fi
    grep -q '^PASS i386 AT_SYSINFO=' "build/i386-p1/sysinfo-$label.out"
done
echo 'PASS i386 AT_SYSINFO proxy/trap equivalence, file syscalls, signals, threads, fork and exec'

musl_libc=${MATON_V3_I386_MUSL_LIBC:-}
musl_interpreter=${MATON_V3_I386_MUSL_INTERPRETER:-}
if [[ -n $musl_libc && -n $musl_interpreter && -f $musl_libc && -f $musl_interpreter ]]; then
    ld -m elf_i386 -o build/i386-p1/sysinfo-musl --dynamic-linker "$musl_interpreter" \
        build/i386-p1/sysinfo-start.o build/i386-p1/sysinfo-entry.o \
        build/i386-p1/sysinfo.o "$musl_libc"
    for rewrite in 0 1; do
        MATON_PROXY_REWRITE=$rewrite MATON_I386_DLOPEN=libc.so timeout 45 "$loader" --android-filter \
            --map "$PWD/build/i386-p1/fixture" /i386-fastpath-file \
            --map "$PWD/build/i386-p1/fastlink" /i386-fastpath-link \
            "$PWD/build/i386-p1/sysinfo-musl" \
            >"build/i386-p1/sysinfo-musl-$rewrite.out" \
            2>"build/i386-p1/sysinfo-musl-$rewrite.log"
        grep -q '^PASS i386 AT_SYSINFO=' "build/i386-p1/sysinfo-musl-$rewrite.out"
    done
    echo 'PASS i386 musl AT_SYSINFO proxy/trap equivalence, signals, threads, fork and exec'
else
    echo 'SKIP i386 musl AT_SYSINFO probe: set MATON_V3_I386_MUSL_LIBC and MATON_V3_I386_MUSL_INTERPRETER'
fi

# Compare the kernel vDSO trampoline, SIGSYS trap route and proxy AT_SYSINFO
# entry using identical raw wrappers and operations.
cc -m32 -nostdinc -fno-stack-protector -fno-pic -O2 -Wall -Wextra -Werror \
    -c tests/i386-sysinfo-bench.c -o build/i386-p1/sysinfo-bench.o
ld -m elf_i386 -o build/i386-p1/sysinfo-bench --dynamic-linker "$interpreter" \
    build/i386-p1/sysinfo-start.o build/i386-p1/sysinfo-entry.o \
    build/i386-p1/sysinfo-bench.o "$libc"
: > build/i386-p1/sysinfo-bench-results.txt
for route in native trapped fast; do
    for run in 1 2 3 4 5; do
        case "$route" in
            native) timeout 30 build/i386-p1/sysinfo-bench "$PWD/build/i386-p1/fixture" 5000 ;;
            trapped|fast)
                rewrite=0; [[ $route == fast ]] && rewrite=1
                MATON_PROXY_REWRITE=$rewrite timeout 30 "$loader" --android-filter \
                    --map "$PWD/build/i386-p1/fixture" /i386-fastpath-file \
                    "$PWD/build/i386-p1/sysinfo-bench" /i386-fastpath-file 5000 ;;
        esac >> build/i386-p1/sysinfo-bench-results.txt
    done
done
cat build/i386-p1/sysinfo-bench-results.txt

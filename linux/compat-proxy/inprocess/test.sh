#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
out=${1:?external output required}
bash "$root/build.sh" "$out"
cc -O2 -Wall -Wextra -Werror "$root/host-test.c" -L"$out" -lmatonos-inprocess -Wl,-rpath,"$out" -o "$out/test"
for mode in static glibc; do
    flags=();[[ $mode != static ]] || flags+=(-static-pie)
    cc -O2 -Wall -Wextra -Werror -pthread "${flags[@]}" "$root/payload.c" -o "$out/$mode-payload"
    timeout 30 "$out/test" "$out/$mode-payload" fixture
    EXPECTED_STATUS=37 timeout 30 "$out/test" "$out/$mode-payload" fixture status37
 done

python3 "$root/build-fixtures.py" "$out/pinned"
timeout 30 "$out/test" "$out/pinned/libmatonos-inprocess-ld.so" --inhibit-cache --library-path "$out/pinned" "$out/pinned/libmatonos-inprocess-glibc.so" fixture
# Fixed-address and cross-class loads must fail without replacing the resident.
cc -O2 -no-pie -pthread "$root/payload.c" -o "$out/fixed-payload"
EXPECTED_STATUS=-95 timeout 30 "$out/test" "$out/fixed-payload" fixture
cat > "$out/peer.S" <<'ASM'
.global _start
_start:
    mov $1,%eax
    xor %ebx,%ebx
    int $0x80
.section .note.GNU-stack,"",@progbits
ASM
cc -m32 -nostdlib -static -Wl,--build-id=none "$out/peer.S" -o "$out/peer"
EXPECTED_STATUS=-8 timeout 30 "$out/test" "$out/peer" fixture

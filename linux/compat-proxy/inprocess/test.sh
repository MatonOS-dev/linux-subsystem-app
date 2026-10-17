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
    INHERITED_FILTER=1 timeout 30 "$out/test" "$out/$mode-payload" fixture
    EXPECTED_STATUS=37 timeout 30 "$out/test" "$out/$mode-payload" fixture status37
    for lifecycle in group-main group-worker thread-main group-race; do
        EXPECTED_STATUS=37 timeout 30 "$out/test" "$out/$mode-payload" fixture "$lifecycle"
    done
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

timeout 30 "$out/test" "$out/pinned/libmatonos-inprocess-ld.so" --inhibit-cache --library-path "$out/pinned" "$out/pinned/libmatonos-inprocess-glibc.so" fixture exec0 "$out/pinned/libmatonos-inprocess-ld.so" "$out/pinned" "$out/pinned/libmatonos-inprocess-glibc.so" "$out/pinned/libmatonos-inprocess-static.so"
# Tiny real STORED ZIP/index fixture for the callable production pack path.
python3 - "$out" <<'PY'
import pathlib,struct,sys,zipfile
out=pathlib.Path(sys.argv[1]); bundle=out/'pack-fixture';bundle.mkdir(exist_ok=True)
with zipfile.ZipFile(bundle/'fixture.zip','w',compression=zipfile.ZIP_STORED) as z:z.writestr('data',b'0123456789abcdef')
def string(s):
 b=s.encode();return struct.pack('<I',len(b))+b
data=b'MTPKIDX1'+struct.pack('<I',2)
for path,kind,mode,offset,length in [('/','dir',0o40755,0,0),('/data','file',0o100644,34,16)]:
 data+=b''.join(string(s) for s in [path,'0'*64,'fixture.zip' if kind=='file' else '',kind,''])+struct.pack('<IQQIII',0,offset,length,0,0,mode)
(bundle/'objects.index').write_bytes(data)
PY
cc -O2 -Wall -Wextra -Werror -pthread "$root/session-test.c" -L"$out" -lmatonos-inprocess -Wl,-rpath,"$out" -o "$out/session-test"
timeout 30 "$out/session-test" "$out/pack-fixture/objects.index" "$out/pack-fixture"

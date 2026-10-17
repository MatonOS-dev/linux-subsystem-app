#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
fixture=$(mktemp -d "$PWD/build/placeholder-raw.XXXXXX")
trap 'rm -r "$fixture"' EXIT
touch "$fixture/.v3-store-root" "$fixture/ordinary"
printf stray >"$fixture/stray"
chmod 1600 "$fixture/stray"
timeout 20 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$fixture" --placeholders
python3 - "$fixture" <<'PYRAW'
import os, pathlib, stat, sys, ctypes, struct
root=pathlib.Path(sys.argv[1]);store=root/'.v3-inodes'
objects=[]
for name in ('a','b'):
    p=root/name
    # These syscalls run outside the proxy: there is no type masking.
    fd=os.open(p,os.O_PATH|os.O_NOFOLLOW)
    st=os.fstat(fd)
    sx=ctypes.create_string_buffer(256)
    assert ctypes.CDLL(None,use_errno=True).statx(fd,b'',0x1000|0x100,0x7ff,sx)==0
    assert stat.S_ISREG(struct.unpack_from('H',sx.raw,28)[0])
    assert struct.unpack_from('Q',sx.raw,40)[0]==0
    os.close(fd)
    assert stat.S_ISREG(st.st_mode) and st.st_mode&stat.S_ISVTX and st.st_size==0
    try: os.readlink(p)
    except OSError as e: assert e.errno==22
    else: raise AssertionError('placeholder is a symlink')
    key=f'p-{st.st_dev:016x}-{st.st_ino:016x}'
    obj=store/(store/key).read_bytes().rstrip(b'\0').decode()
    objects.append(obj)
    assert obj.read_bytes()==b'content' and not obj.stat().st_mode&stat.S_ISVTX
assert objects[0]==objects[1]
assert (root/'stray').read_bytes()==b'stray' and (root/'stray').stat().st_mode&stat.S_ISVTX
assert not (root/'ordinary').stat().st_mode&stat.S_ISVTX
print('PASS raw O_PATH|O_NOFOLLOW regular placeholders, EINVAL readlink, sticky index and shared backing')
PYRAW
outside=$(mktemp "$PWD/build/outside.XXXXXX")
rm "$outside"
# Even a host permitting native hardlinks must not export an empty placeholder.
timeout 20 "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$fixture" --escape "$outside"
test ! -e "$outside"
printf new >"$outside"
timeout 20 build/link-deny "$PWD/build/linux-layer-v3" --android-filter "$PWD/build/hardlinks-test" "$fixture" --replace "$outside"
test ! -e "$outside"
test "$(find "$fixture/.v3-inodes" -name 'p-*' | wc -l)" -eq 1
printf 'PASS incoming ordinary rename retires the overwritten placeholder index\n'

#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/repo"
cat >"$tmp/bin/ostree" <<'PY'
#!/usr/bin/env python3
import os, pathlib, shutil, sys
a=sys.argv[1:]
if a[0]=='checkout':
    commit=a[-2]; dest=pathlib.Path(a[-1]); src=pathlib.Path(os.environ['FIXTURE_ROOT'])/commit
    shutil.copytree(src,dest)
elif a[0]=='show': print('fixture commit '+a[-1])
else: raise SystemExit('unsupported fixture ostree command: '+a[0])
PY
chmod +x "$tmp/bin/ostree"
export PATH="$tmp/bin:$PATH" FIXTURE_ROOT="$tmp/commits"
mkdir -p "$FIXTURE_ROOT/c1/usr/bin" "$FIXTURE_ROOT/c2/usr/bin"
python3 - "$FIXTURE_ROOT" <<'PY'
import pathlib,sys
r=pathlib.Path(sys.argv[1]);
(r/'c1/usr/bin/a').write_bytes(b'A'*8192); (r/'c1/usr/bin/b').write_bytes(b'B'*8192); (r/'c1/usr/bin/c').write_bytes(b'C'*8192)
(r/'c2/usr/bin/a').write_bytes(b'A'*8192); (r/'c2/usr/bin/b').write_bytes(b'D'*8192); (r/'c2/usr/bin/c').write_bytes(b'E'*8192)
PY
python3 pack.py pack --repo "$tmp/repo" --commit c1 --out "$tmp/first" --pack-size 20000
python3 pack.py update --repo "$tmp/repo" --commit c2 --old-index "$tmp/first/objects.index" --old-packs "$tmp/first" --out "$tmp/u50" --dead-ratio .5
python3 pack.py update --repo "$tmp/repo" --commit c2 --old-index "$tmp/first/objects.index" --old-packs "$tmp/first" --out "$tmp/u75" --dead-ratio .75
python3 pack.py update --repo "$tmp/repo" --commit c2 --old-index "$tmp/first/objects.index" --old-packs "$tmp/first" --out "$tmp/uall" --dead-ratio 0
python3 - "$tmp" <<'PY'
import pathlib,sys,zipfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
import pack
root=pathlib.Path(sys.argv[1]); rec=pack.read_index(root/'first/objects.index')
assert len(rec)==3
for p in (root/'first').glob('pack-*.zip'):
  with zipfile.ZipFile(p) as z:
    for i in z.infolist(): assert (i.header_offset+30+len(i.filename.encode())+len(i.extra))%4096==0
assert len(pack.read_index(root/'u50/objects.index'))==3
assert len(pack.read_index(root/'u75/objects.index'))==3
assert len(pack.read_index(root/'uall/objects.index'))==3
PY
cc -std=c99 -Wall -Wextra -Werror -I. reader.c tests/reader_test.c -o "$tmp/reader-test"
"$tmp/reader-test" "$tmp/first/objects.index"
echo 'OSTree pack tests passed (generated two-commit fixture; threshold replay: .5, .75, full touched-pack rewrite)'

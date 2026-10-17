#!/usr/bin/env bash
# Real repository lifecycle, with native linkat denied throughout.
set -euo pipefail
cd "$(dirname "$0")/.."
if ! command -v ostree >/dev/null; then echo 'SKIP host ostree unavailable'; exit 0; fi
fixture=$(mktemp -d "$PWD/build/ostree-links.XXXXXX")
trap 'rm -r "$fixture"' EXIT
touch "$fixture/.v3-store-root"
mkdir "$fixture/input"
printf 'metadata content\n' >"$fixture/input/metadata"
run() { timeout 60 build/link-deny "$PWD/build/linux-layer-v3" --android-filter /usr/bin/ostree --repo="$fixture/repo" "$@"; }
run init --mode=bare-user-only
run commit --branch=test --subject=test --tree="dir=$fixture/input"
run checkout --user-mode --require-hardlinks test "$fixture/checkout"
run fsck
python3 - "$fixture" <<'PY'
import pathlib,stat,sys
root=pathlib.Path(sys.argv[1]);p=root/'checkout/metadata';st=p.stat()
assert stat.S_ISREG(st.st_mode) and st.st_mode&stat.S_ISVTX and st.st_size==0
store=root/'.v3-inodes';obj=store/(store/f'p-{st.st_dev:016x}-{st.st_ino:016x}').read_bytes().rstrip(b'\0').decode()
assert obj.read_bytes()==b'metadata content\n'
assert int.from_bytes(pathlib.Path(str(obj)+'.count').read_bytes(),'little')>=2
print('PASS host ostree init/commit/require-hardlinks checkout/fsck with indexed shared backing')
PY

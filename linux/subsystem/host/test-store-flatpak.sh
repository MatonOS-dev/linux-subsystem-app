#!/usr/bin/env bash
# Tiny local repository: exercise the same resolver, full refs and exact sizes without downloads.
set -euo pipefail
binary=$(realpath "${1:?usage: test-store-flatpak.sh MULTICALL PLAN_HELPER OUTPUT}")
helper=$(realpath "${2:?}")
output=$(realpath -m "${3:?}")
mkdir -p "$output"/runtime/files "$output"/runtime/usr/share "$output"/app/files/bin "$output"/home
export HOME="$output/home" XDG_CACHE_HOME="$output/cache" XDG_DATA_HOME="$output/data" XDG_CONFIG_HOME="$output/config"
export FLATPAK_USER_DIR="$output/installation" FLATPAK_SYSTEM_DIR="$output/installation"
flatpak() { "$binary" flatpak "$@"; }
cat > "$output/runtime/metadata" <<'META'
[Runtime]
name=org.matonos.StoreTest.Platform
runtime=org.matonos.StoreTest.Platform/x86_64/1
sdk=org.matonos.StoreTest.Platform/x86_64/1
META
cat > "$output/app/metadata" <<'META'
[Application]
name=org.matonos.StoreTest.App
runtime=org.matonos.StoreTest.Platform/x86_64/1
sdk=org.matonos.StoreTest.Platform/x86_64/1
command=hello
META
printf '#!/bin/sh\nexit 0\n' > "$output/app/files/bin/hello"
chmod +x "$output/app/files/bin/hello"
printf 'runtime fixture\n' > "$output/runtime/usr/share/fixture"
flatpak build-export --runtime --arch=x86_64 "$output/repo" "$output/runtime" 1
flatpak build-finish "$output/app"
flatpak build-export --arch=x86_64 "$output/repo" "$output/app" stable
flatpak remote-add --user --no-gpg-verify --if-not-exists store-test "$output/repo"
flatpak install --user --noninteractive --assumeyes store-test app/org.matonos.StoreTest.App/x86_64/stable
"$helper" --installed "$output/installation" > "$output/installed.tsv"
"$helper" --sizes "$output/installation" store-test app/org.matonos.StoreTest.App/x86_64/stable > "$output/sizes.txt"
"$helper" --updates "$output/installation" > "$output/before-updates.tsv"
printf '# updated fixture\n' >> "$output/app/files/bin/hello"
flatpak build-export --arch=x86_64 "$output/repo" "$output/app" stable
"$helper" --updates "$output/installation" > "$output/after-updates.tsv"
python3 - "$output" "$helper" <<'PY'
import pathlib,sys
out=pathlib.Path(sys.argv[1])
rows=dict(line.split('\t') for line in (out/'installed.tsv').read_text().splitlines())
assert rows=={'app/org.matonos.StoreTest.App/x86_64/stable':'store-test','runtime/org.matonos.StoreTest.Platform/x86_64/1':'store-test'},rows
sizes=list(map(int,(out/'sizes.txt').read_text().splitlines()))
assert len(sizes)==2 and all(size>0 for size in sizes),sizes
assert not (out/'before-updates.tsv').read_text().strip(), 'unexpected update before publishing'
assert (out/'after-updates.tsv').read_text().split('\t')[0]=='app/org.matonos.StoreTest.App/x86_64/stable', 'new commit not detected'
# Model signed DeploymentRecord.materialize: no payload files are needed for queries.
import shutil,os,subprocess
snapshot=out/'snapshot';(snapshot/'repo').mkdir(parents=True,exist_ok=True)
shutil.copyfile(out/'installation/repo/config',snapshot/'repo/config')
for ref,remote in rows.items():
 branch=out/'installation'/ref;source=(branch/'active').resolve();commit=source.name.split('-')[0]
 version=snapshot/ref/source.name;version.mkdir(parents=True,exist_ok=True);(version/'files').mkdir(exist_ok=True)
 for name in ('metadata','deploy'):shutil.copyfile(source/name,version/name)
 (version/'.ref').touch();(snapshot/ref/'active').symlink_to(source.name)
 kind,app,arch,track=ref.split('/');(snapshot/kind/app/'current').symlink_to(arch+'/'+track)
 object_path=pathlib.Path('repo/objects')/commit[:2]/(commit[2:]+'.commit')
 (snapshot/object_path).parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(out/'installation'/object_path,snapshot/object_path)
 deployed=snapshot/'repo/refs/heads/deploy'/ref;deployed.parent.mkdir(parents=True,exist_ok=True);deployed.write_text(commit+'\n')
(snapshot/'repo/tmp').mkdir(exist_ok=True);(snapshot/'repo/refs/remotes').mkdir(parents=True,exist_ok=True)
helper=sys.argv[2]
assert subprocess.check_output([helper,'--updates',str(snapshot)],text=True).split('\t')[0]=='app/org.matonos.StoreTest.App/x86_64/stable'
print('Store Flatpak integration: PASS (native resolver, complete refs, exact sizes, real update before/after publishing, metadata-only installed snapshot)')
PY

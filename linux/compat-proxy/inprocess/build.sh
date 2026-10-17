#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?external output directory required}
compiler=${INPROCESS_CC:-cc}
mkdir -p "$out"
# Generate policy into a disposable source mirror; the standalone suites use
# the same generated headers. Never put build output in the worktree.
work=$out/source
mkdir -p "$work"
cp -a "$root"/*.c "$root"/*.h "$root/arch" "$root/gen-policy.py" "$work/"
mkdir -p "$out/packs"
cp -a "$root/../packs/." "$out/packs/"
python3 "$work/gen-policy.py"
for name in loader proxy vfs links net sysvipc state; do
    "$compiler" -DPROXY_LIBRARY -DMATON_IN_PROCESS -fPIC -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c "$work/$name.c" -o "$out/$name.o"
done
"$compiler" -DMATON_IN_PROCESS -fPIC -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c "$root/arch/x86_64/thread-adopt.c" -o "$out/adoption.o"
"$compiler" -fPIC -c "$root/arch/x86_64/raw.S" -o "$out/raw.o"
"$compiler" -fPIC -c "$root/arch/x86_64/adopt.S" -o "$out/adopt.o"
"$compiler" -fPIC -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror -c "$root/inprocess/routing.c" -o "$out/routing.o"
extra=()
if [[ ${INPROCESS_ANDROID:-0} == 1 ]]; then
    "$compiler" -fPIC -O2 -Wall -Wextra -Werror -c "$root/inprocess/jni.c" -o "$out/jni.o"
    extra+=("$out/jni.o")
fi
"$compiler" -shared -Wl,-Bsymbolic,-z,now "${extra[@]}" "$out"/{loader,proxy,vfs,links,net,sysvipc,state,adoption,raw,adopt,routing}.o -pthread -ldl -o "$out/libmatonos-inprocess.so"

#!/usr/bin/env bash
# Host per-call adoption cost for the ICD forwarder's switch_to_android /
# switch_back. Keep the legacy test launcher build in external output so it
# cannot create generated files or binaries in the source worktree.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:?external output directory}
limit=${MATON_SWITCH_BENCH_NS:-40}
src="$out/launcher-src/linux"
mkdir -p "$out" "$src"
# Preserve sibling includes expected by the old launcher test harness. The
# canonical production ABI now lives beside the compatibility include shims.
for tree in gpu-forward-spike gpu-bridge-common compat-proxy packs; do
  rm -rf "$src/$tree"
  cp -a "$root/linux/$tree" "$src/$tree"
done
spike=$src/gpu-forward-spike
proxy=$src/compat-proxy
bash "$spike/build.sh" >/dev/null
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -Wl,-z,now -DMATON_ADOPT \
   "$spike/launcher.c" "$proxy/arch/x86_64/thread-adopt.c" "$proxy/adopt-map.c" \
   "$proxy/arch/x86_64/adopt.S" "$proxy/arch/x86_64/altstack.S" \
   "$spike"/build/{loader,proxy,vfs,links,net,sysvipc,state,raw}.o -pthread -ldl \
   -o "$out/launcher-adopt"
cc=${CC:-clang-21}
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin -fno-stack-protector \
      -I"$here" -I"$here/arch/x86_64" \
      "$here/tests/switch-bench.c" "$here/runtime.c" -o "$out/switch-bench"
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 300s \
    "$out/launcher-adopt" "$out/switch-bench" | tee "$out/switch-bench.out"
python3 - "$out/switch-bench.out" "$limit" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
limit = float(sys.argv[2])
wanted = ['ROUNDTRIP_1T', 'ROUNDTRIP_1T_LOADED', 'ROUNDTRIP_64T']
values = {}
for key in wanted:
    match = re.search(r'^%s\s+([0-9.]+)\s*$' % key, text, re.M)
    if not match:
        sys.exit('missing %s in switch-bench output' % key)
    values[key] = float(match.group(1))
for key in wanted:
    if values[key] > limit:
        sys.exit('%s is %.1f ns/call, over the %.0f ns host budget' % (key, values[key], limit))
if values['ROUNDTRIP_1T_LOADED'] > values['ROUNDTRIP_1T'] * 1.5:
    sys.exit('lookup grows with live threads: %.1f -> %.1f ns/call' %
             (values['ROUNDTRIP_1T'], values['ROUNDTRIP_1T_LOADED']))
print('PASS switch round trip under %.0f ns: 1T %.1f, 1T+64 live %.1f, 64T %.1f ns/call' %
      (limit, values['ROUNDTRIP_1T'], values['ROUNDTRIP_1T_LOADED'], values['ROUNDTRIP_64T']))
PY

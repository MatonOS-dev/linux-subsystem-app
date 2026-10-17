#!/usr/bin/env bash
# Host per-call adoption cost for the ICD forwarder's switch_to_android /
# switch_back. The provider keeps the FS-base lookup O(1) in the number of live
# adopted threads, so the round trip must stay flat from 1 to 64 threads and
# under the host budget. Runs the benchmark under the adoption launcher and
# fails if any reported round trip exceeds the limit.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
spike=$here/../gpu-forward-spike
proxy=$here/../compat-proxy
out=${1:?external output directory}
limit=${MATON_SWITCH_BENCH_NS:-40}
mkdir -p "$out"
# The adoption launcher reuses the proxy objects and the generated app policy.
bash "$spike/build.sh" >/dev/null
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -Wl,-z,now -DMATON_ADOPT \
   "$spike/launcher.c" "$proxy/arch/x86_64/thread-adopt.c" "$proxy/adopt-map.c" \
   "$proxy/arch/x86_64/adopt.S" \
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
# The lookup is keyed on the FS base; 64 live bindings must not slow a call.
if values['ROUNDTRIP_1T_LOADED'] > values['ROUNDTRIP_1T'] * 1.5:
    sys.exit('lookup grows with live threads: %.1f -> %.1f ns/call'
             % (values['ROUNDTRIP_1T'], values['ROUNDTRIP_1T_LOADED']))
print('PASS switch round trip under %.0f ns: 1T %.1f, 1T+64 live %.1f, 64T %.1f ns/call'
      % (limit, values['ROUNDTRIP_1T'], values['ROUNDTRIP_1T_LOADED'], values['ROUNDTRIP_64T']))
PY

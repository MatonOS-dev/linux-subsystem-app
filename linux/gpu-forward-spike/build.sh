#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ $(uname -m) != x86_64 ]]; then
  echo "This TLS probe supports x86_64 only" >&2; exit 2
fi
mkdir -p build
proxy=../compat-proxy
python3 "$proxy/gen-policy.py"
compiler=${SPIKE_CC:-cc}
# Same proxy sources, separate outputs; never writes to the AOSP tree.
for source in loader proxy vfs links net sysvipc state; do
  "$compiler" -DPROXY_LIBRARY -O2 -fno-stack-protector -fno-builtin -fPIC -Wall -Wextra -Werror -c "$proxy/$source.c" -o "build/$source.o"
done
"$compiler" -c "$proxy/arch/x86_64/raw.S" -o build/raw.o
"$compiler" -O2 -fPIC -fno-stack-protector -pie launcher.c build/*.o -ldl -o build/launcher
cc -I"${MATON_AOSP_ROOT:-/mnt/data/aosp}/external/vulkan-headers/include" -O2 -fno-stack-protector -Wall -Wextra -Werror probe.c -ldl -pthread -o build/probe

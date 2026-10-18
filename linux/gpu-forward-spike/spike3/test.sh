#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z ${SPIKE3_LOADER:-} ]]; then
  echo 'SKIP resident adoption suite: provide a separate SPIKE3_LOADER and SPIKE3_LIBRARY_PATH; using host libc for both resident and payload invalidates the libc-IP exemption test'
  exit 0
fi
bash build.sh
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -Wl,-z,now -DMATON_ADOPT launcher.c ../compat-proxy/arch/x86_64/thread-adopt.c ../compat-proxy/adopt-map.c ../compat-proxy/arch/x86_64/adopt.S build/{loader,proxy,vfs,links,net,sysvipc,state,raw}.o -pthread -ldl -o build/launcher-adopt
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector spike3/fork-test.c spike3/thunk.c -pthread -o build/adopt-fork-test
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector spike3/test.c spike3/thunk.c -pthread -lm -o build/adopt-test
launch=("$PWD/build/launcher-adopt")
if [[ -n ${SPIKE3_LOADER:-} ]]; then
  : "${SPIKE3_LIBRARY_PATH:?runtime library path required}"
  launch+=("$SPIKE3_LOADER" --inhibit-cache --library-path "$SPIKE3_LIBRARY_PATH")
fi
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 30s "${launch[@]}" "$PWD/build/adopt-test"
MATON_ADOPT_FORCE_CLONE=1 env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 30s "${launch[@]}" "$PWD/build/adopt-test"
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 30s "${launch[@]}" "$PWD/build/adopt-test" main-exit
set +e
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 30s "${launch[@]}" "$PWD/build/adopt-test" exit-group
status=$?
set -e
[[ $status == 73 ]]
echo 'PASS: adopted worker exit_group status=73'
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s "${launch[@]}" "$PWD/build/adopt-test" many
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s "${launch[@]}" "$PWD/build/adopt-fork-test"
MATON_ADOPT_FORCE_CLONE=1 env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s "${launch[@]}" "$PWD/build/adopt-fork-test"
if command -v strace >/dev/null; then
  trace_dir=$(mktemp -d "$PWD/build/adopt-trace.XXXXXX")
  env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s strace -ff -o "$trace_dir/task" "${launch[@]}" "$PWD/build/adopt-fork-test"
  python3 spike3/check-vfork-trace.py "$trace_dir/task"
  trace_dir=$(mktemp -d "$PWD/build/adopt-trace.XXXXXX")
  MATON_ADOPT_FORCE_CLONE=1 env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s strace -ff -o "$trace_dir/task" "${launch[@]}" "$PWD/build/adopt-fork-test"
  python3 spike3/check-vfork-trace.py "$trace_dir/task"
fi
if [[ ${SPIKE3_SKIP_PORTABILITY:-0} != 1 ]]; then
  cc -nostdlib -static -no-pie -O2 -g -fno-builtin -fno-stack-protector -Wall -Wextra -Werror spike3/raw-test.c spike3/raw-test.S -o build/adopt-raw-test
  [[ -z $(nm -u build/adopt-raw-test) ]]
  timeout 30s build/launcher-adopt "$PWD/build/adopt-raw-test" 2>build/raw-rejections.log
  [[ $(wc -l < build/raw-rejections.log) == 9 ]]
  cc -O2 -g -fno-stack-protector -Wall -Wextra -Werror spike3/libc-thread-test.c spike3/thunk.c -pthread -o build/adopt-libc-test
  timeout 30s "${launch[@]}" "$PWD/build/adopt-libc-test"
  musl_cc=${MUSL_CC:-$(command -v musl-gcc || bash spike3/setup-musl.sh)}
  "$musl_cc" -static -O2 -g -Wall -Wextra -Werror -fno-stack-protector spike3/libc-thread-test.c spike3/thunk.c -pthread -o build/adopt-musl-test
  timeout 30s build/launcher-adopt "$PWD/build/adopt-musl-test"
fi

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash build.sh
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -Wl,-z,now -DMATON_ADOPT launcher.c ../compat-proxy/arch/x86_64/thread-adopt.c ../compat-proxy/arch/x86_64/adopt.S build/{loader,proxy,vfs,links,net,sysvipc,state,raw}.o -pthread -ldl -o build/launcher-adopt
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector spike3/fork-test.c spike3/thunk.c -pthread -o build/adopt-fork-test
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector spike3/test.c spike3/thunk.c -pthread -lm -o build/adopt-test
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 30s build/launcher-adopt "$PWD/build/adopt-test"
MATON_ADOPT_FORCE_CLONE=1 env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 30s build/launcher-adopt "$PWD/build/adopt-test"
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 30s build/launcher-adopt "$PWD/build/adopt-test" main-exit
set +e
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 30s build/launcher-adopt "$PWD/build/adopt-test" exit-group
status=$?
set -e
[[ $status == 73 ]]
echo 'PASS: adopted worker exit_group status=73'
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s build/launcher-adopt "$PWD/build/adopt-test" many
env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s build/launcher-adopt "$PWD/build/adopt-fork-test"
MATON_ADOPT_FORCE_CLONE=1 env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s build/launcher-adopt "$PWD/build/adopt-fork-test"
if command -v strace >/dev/null; then
  trace_dir=$(mktemp -d "$PWD/build/adopt-trace.XXXXXX")
  env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s strace -ff -o "$trace_dir/task" build/launcher-adopt "$PWD/build/adopt-fork-test"
  python3 spike3/check-vfork-trace.py "$trace_dir/task"
  trace_dir=$(mktemp -d "$PWD/build/adopt-trace.XXXXXX")
  MATON_ADOPT_FORCE_CLONE=1 env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0 timeout 60s strace -ff -o "$trace_dir/task" build/launcher-adopt "$PWD/build/adopt-fork-test"
  python3 spike3/check-vfork-trace.py "$trace_dir/task"
fi

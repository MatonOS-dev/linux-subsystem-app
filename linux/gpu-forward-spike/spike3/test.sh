#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash build.sh
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -Wl,-z,now -DMATON_ADOPT launcher.c ../compat-proxy/arch/x86_64/thread-adopt.c ../compat-proxy/arch/x86_64/adopt.S build/{loader,proxy,vfs,links,net,sysvipc,state,raw}.o -pthread -ldl -o build/launcher-adopt
cc -O2 -g -Wall -Wextra -Werror -fno-stack-protector spike3/test.c spike3/thunk.c -pthread -lm -o build/adopt-test
timeout 30s build/launcher-adopt "$PWD/build/adopt-test"
MATON_ADOPT_FORCE_CLONE=1 timeout 30s build/launcher-adopt "$PWD/build/adopt-test"
timeout 30s build/launcher-adopt "$PWD/build/adopt-test" main-exit
set +e
timeout 30s build/launcher-adopt "$PWD/build/adopt-test" exit-group
status=$?
set -e
[[ $status == 73 ]]
echo 'PASS: adopted worker exit_group status=73'

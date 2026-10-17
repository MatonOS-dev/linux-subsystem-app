#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang
SPIKE_CC="$compiler" bash build.sh
"$compiler" -O2 -g -Wall -Wextra -Werror -fno-stack-protector -fno-builtin -Wl,-z,now -DMATON_ADOPT launcher.c ../compat-proxy/arch/x86_64/thread-adopt.c ../compat-proxy/adopt-map.c ../compat-proxy/arch/x86_64/adopt.S build/{loader,proxy,vfs,links,net,sysvipc,state,raw}.o -pthread -ldl -o build/launcher-adopt-android

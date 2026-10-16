#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang
SPIKE_CC="$compiler" bash build.sh
mv build/launcher build/launcher-android
"$compiler" -O2 -Wall -Wextra -Werror egl-probe.c -lEGL -o build/egl-probe-android

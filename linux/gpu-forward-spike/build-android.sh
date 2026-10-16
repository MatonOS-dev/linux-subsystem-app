#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ndk=${MATON_ANDROID_NDK:-${MATON_NDK:-$HOME/Documents/android-ndk-r30}}
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang
SPIKE_CC="$compiler" bash build.sh
mv build/launcher build/launcher-android
"$compiler" -O2 -Wall -Wextra -Werror egl-probe.c -lEGL -o build/egl-probe-android
aosp=${MATON_AOSP_ROOT:-/mnt/data/aosp}
mkdir -p build/spike2
python3 spike2/gen-thunks.py "$aosp/external/vulkan-headers/registry/vk.xml" > build/spike2/thunks.c
"$compiler" -O2 -fno-stack-protector -fPIC -Wall -Wextra -Werror -Ispike2 -I"$aosp/external/vulkan-headers/include" -c build/spike2/thunks.c -o build/spike2/thunks-android.o
"$compiler" -O2 -fno-stack-protector -Wall -Wextra -Werror spike2/donor-control.c spike2/provider.c spike2/switch.c -o build/spike2/donor-control-android

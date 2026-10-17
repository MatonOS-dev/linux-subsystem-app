#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
aosp=${MATON_AOSP_ROOT:-/mnt/data/aosp}
mkdir -p build/spike2
python3 spike2/gen-thunks.py "$aosp/external/vulkan-headers/registry/vk.xml" > build/spike2/thunks.c
cc -O2 -fno-stack-protector -fPIC -Wall -Wextra -Werror -Ispike2 -I"$aosp/external/vulkan-headers/include" spike2/test.c spike2/switch.c spike2/client.c build/spike2/thunks.c -pthread -o build/spike2/test
build/spike2/test
build/spike2/test fallback
build/launcher "$PWD/build/spike2/test"
build/launcher "$PWD/build/spike2/test" fallback
cc -O2 -fno-stack-protector -Wall -Wextra -Werror -I"$aosp/external/vulkan-headers/include" spike2/vulkan-probe.c spike2/switch.c spike2/client.c -pthread -o build/spike2/vulkan-probe

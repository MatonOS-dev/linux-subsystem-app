#!/usr/bin/env bash
# Requires stage-B glibc runtime/loader under /data/local/tmp/vk/lib.
set -euo pipefail
out=${1:?stage C external output}
adb=${ADB:-adb}; serial=${ADB_SERIAL:-localhost:5556}
a() { "$adb" -s "$serial" "$@"; }
a shell mkdir -p /data/local/tmp/vk/c-musl /data/local/tmp/vk/c-zink
a push "$out/launcher-src/linux/gpu-forward-spike/build/launcher-adopt-android" /data/local/tmp/vk/launcher-c
a push "$out/build/libmaton_vulkan.so" /data/local/tmp/vk/libmaton_vulkan.so
a push "$out/tests/offscreen" "$out/build/switch-bench" /data/local/tmp/vk/
a push "$out/musl/lib/libc.so" "$out/musl-loader-build/loader/libvulkan.so.1.4.341" "$out/tests/offscreen-musl" /data/local/tmp/vk/c-musl/
a push "$out/mesa/lib/x86_64-linux-gnu/libEGL.so.1.0.0" "$out/mesa/lib/x86_64-linux-gnu/libGLESv2.so.2.0.0" "$out/mesa/lib/x86_64-linux-gnu/libgallium-26.2.3.so" "$out/deps/lib/x86_64-linux-gnu/libdrm.so.2.134.0" /lib/x86_64-linux-gnu/libexpat.so.1 /lib/x86_64-linux-gnu/libz.so.1 /lib/x86_64-linux-gnu/libzstd.so.1 "$out/tests/zink-offscreen" /data/local/tmp/vk/c-zink/
a shell 'cd /data/local/tmp/vk; ln -sf libvulkan.so.1.4.341 c-musl/libvulkan.so.1; ln -sf libEGL.so.1.0.0 c-zink/libEGL.so.1; ln -sf libGLESv2.so.2.0.0 c-zink/libGLESv2.so.2; ln -sf libdrm.so.2.134.0 c-zink/libdrm.so.2'
a shell 'cd /data/local/tmp/vk; VK_ICD_FILENAMES=/data/local/tmp/vk/maton.json ./launcher-c ./c-musl/libc.so --library-path /data/local/tmp/vk/c-musl ./c-musl/offscreen-musl' > "$out/musl-vm.txt" 2>&1
a shell 'cd /data/local/tmp/vk; VK_ICD_FILENAMES=/data/local/tmp/vk/maton.json ./launcher-c ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./offscreen' > "$out/glibc-vm.txt" 2>&1
a shell 'cd /data/local/tmp/vk; VK_ICD_FILENAMES=/data/local/tmp/vk/maton.json ./launcher-c ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./vulkaninfo --summary' > "$out/vulkaninfo-vm.txt" 2>&1
a shell 'cd /data/local/tmp/vk; ./launcher-c ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./switch-bench' > "$out/switch-vm.txt" 2>&1
for mode in core compat es; do
 a shell "cd /data/local/tmp/vk; VK_ICD_FILENAMES=/data/local/tmp/vk/maton.json MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1 MESA_SHADER_CACHE_DISABLE=true ./launcher-c ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/c-zink:/data/local/tmp/vk/lib ./c-zink/zink-offscreen $mode" > "$out/zink-$mode-vm.txt" 2>&1
done
cat "$out/musl-vm.txt" "$out/glibc-vm.txt" "$out/zink-core-vm.txt" "$out/zink-compat-vm.txt" "$out/zink-es-vm.txt" "$out/switch-vm.txt"

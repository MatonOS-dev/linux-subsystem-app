#!/usr/bin/env bash
set -euo pipefail
out=${1:?stage C output}; adb=${ADB:-adb}; serial=${ADB_SERIAL:-localhost:5556}
a() { "$adb" -s "$serial" "$@"; }
a push "$out/tests/zink-check-profiles" "$out/tests/zink-check-profiles-offscreen" "$out/tests/zink-check-android" "$out/tests/driver-probe" /data/local/tmp/vk/
a shell /data/local/tmp/vk/zink-check-android > "$out/profiles-android.txt" 2>&1
a shell /data/local/tmp/vk/driver-probe > "$out/driver-extensions.txt" 2>&1
for variant in profiles profiles-offscreen; do
 name=profiles-forwarded
 [[ "$variant" != profiles-offscreen ]] || name=profiles-offscreen-forwarded
 a shell "cd /data/local/tmp/vk; VK_ICD_FILENAMES=/data/local/tmp/vk/maton.json ./launcher-c ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./zink-check-$variant" > "$out/$name.txt" 2>&1
done
python3 "$(dirname "$0")/audit-zink.py" "$out/src/mesa-26.2.3" "$out/driver-extensions.txt" > "$out/zink-extension-audit.md"

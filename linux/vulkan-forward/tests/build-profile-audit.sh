#!/usr/bin/env bash
# Upstream Zink profiles checker, plus an explicitly WSI-free diagnostic copy.
set -euo pipefail
out=${1:?external stage-C output}; deps=${2:?stage-A output}
case "$out" in /*) ;; *) exit 2;; esac
repo=$(git rev-parse --show-toplevel)
case "$(realpath -m "$out")/" in "$repo/"*) exit 2;; esac
archive="$out/src/profiles.zip"
[[ -f "$archive" ]] || curl -fL --connect-timeout 15 --max-time 180 https://codeload.github.com/KhronosGroup/Vulkan-Profiles/zip/a1e185fb5035f9726d16a869a2f11067dd21aa10 -o "$archive"
echo "775b0faebd53e3f31f1771d7ca9f8d899778ce596177fb9edd8d26d5323f854a  $archive" | sha256sum -c -
[[ -d "$out/src/Vulkan-Profiles-a1e185fb5035f9726d16a869a2f11067dd21aa10" ]] || unzip -q "$archive" -d "$out/src"
mesa="$out/src/mesa-26.2.3"
mkdir -p "$out/tests/profiles" "$out/tests/profiles-offscreen" "$out/tests/profile-json"
python3 - "$mesa" "$out/tests/profile-json" <<'PY'
import json,pathlib,sys
j=json.loads((pathlib.Path(sys.argv[1])/'src/gallium/drivers/zink/VP_ZINK_requirements.json').read_text())
for cap in j['capabilities'].values():
 for name in ['VK_KHR_incremental_present','VK_KHR_swapchain_mutable_format']:
  cap.get('extensions',{}).pop(name,None)
(pathlib.Path(sys.argv[2])/'VP_ZINK_requirements.json').write_text(json.dumps(j,indent=2))
PY
# Thin C++ callback required by upstream C++ Profiles header; route native diagnostics to stderr.
cat > "$out/tests/profile-message.cpp" <<'CPP'
#include <cstdio>
void maton_profile_message(const char *m) { fprintf(stderr,"%s\n",m); }
CPP
for variant in profiles profiles-offscreen; do
 input="$mesa/src/gallium/drivers/zink"
 [[ "$variant" != profiles-offscreen ]] || input="$out/tests/profile-json"
 python3 "$out/src/Vulkan-Profiles-a1e185fb5035f9726d16a869a2f11067dd21aa10/scripts/gen_profiles_solution.py" \
  --input "$input" --input-filenames VP_ZINK_requirements.json --output-library-inc "$out/tests/$variant" \
  --registry "$mesa/src/vulkan/registry/vk.xml" --config debug
 g++ -std=c++17 -O2 -I"$out/tests/$variant" -I"$mesa/include" \
  "$mesa/src/gallium/drivers/zink/check_requirements/zink_check_requirements.cpp" \
  -L"$deps/loader-build/loader" -lvulkan -o "$out/tests/zink-check-$variant"
done
ndk=${MATON_NDK:-$HOME/Documents/android-ndk-r30}
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang++" \
 -std=c++17 -O2 -DVP_DEBUG_MESSAGE_CALLBACK=maton_profile_message -I"$out/tests/profiles" -I"$mesa/include" \
 "$out/tests/profile-message.cpp" "$mesa/src/gallium/drivers/zink/check_requirements/zink_check_requirements.cpp" \
 -lvulkan -static-libstdc++ -o "$out/tests/zink-check-android"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang" \
 -O2 -Wall -Wextra -Werror -I"$deps/src/Vulkan-Headers-1.4.341/include" \
 "$(dirname "$0")/driver-probe.c" -lvulkan -o "$out/tests/driver-probe"

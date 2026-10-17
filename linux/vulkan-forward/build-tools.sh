#!/usr/bin/env bash
# Reproducible stage-A test dependencies, never an AOSP build. At most -j4.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build-tools.sh EXTERNAL_ABSOLUTE_OUTPUT_DIR}
case "$out" in /*) ;; *) exit 2;; esac
repo=$(git -C "$here" rev-parse --show-toplevel)
case "$out/" in "$repo/"*) echo 'use an output directory outside the repo' >&2; exit 2;; esac
mkdir -p "$out/src"
python3 - "$here/pins.json" "$out/src" <<'PY'
import hashlib,json,pathlib,subprocess,sys,tarfile
pins=json.loads(pathlib.Path(sys.argv[1]).read_text()); dest=pathlib.Path(sys.argv[2]); version=pins['version']
for project in ['Vulkan-Headers','Vulkan-Loader','Vulkan-Tools']:
    archive=dest/(project+'.tar.gz')
    if not archive.exists():
        subprocess.run(['curl','-fL','--retry','2',f'https://codeload.github.com/KhronosGroup/{project}/tar.gz/refs/tags/v{version}','-o',str(archive)],check=True)
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==pins[project],project+' checksum mismatch'
    if not (dest/(project+'-'+version)).exists():
        with tarfile.open(archive) as source: source.extractall(dest,filter='data')
PY
cmake=${CMAKE:-cmake}
"$cmake" -S "$out/src/Vulkan-Headers-1.4.341" -B "$out/headers-build" -DCMAKE_INSTALL_PREFIX="$out/deps" -DVULKAN_HEADERS_ENABLE_TESTS=OFF
"$cmake" --install "$out/headers-build"
"$cmake" -S "$out/src/Vulkan-Loader-1.4.341" -B "$out/loader-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$out/deps" -DBUILD_TESTS=OFF -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF -DBUILD_WSI_WAYLAND_SUPPORT=OFF -DBUILD_WSI_DIRECTFB_SUPPORT=OFF -DUPDATE_DEPS=OFF
"$cmake" --build "$out/loader-build" -j4
"$cmake" -S "$out/src/Vulkan-Tools-1.4.341" -B "$out/tools-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$out/deps" -DBUILD_CUBE=OFF -DBUILD_ICD=OFF -DBUILD_TESTS=OFF -DBUILD_WSI_XCB_SUPPORT=OFF -DBUILD_WSI_XLIB_SUPPORT=OFF -DBUILD_WSI_WAYLAND_SUPPORT=OFF
"$cmake" --build "$out/tools-build" -j4

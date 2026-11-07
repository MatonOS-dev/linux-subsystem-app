#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?external output}
case "$out" in /*) ;; *) exit 2;; esac
repo=$(git -C "$here" rev-parse --show-toplevel)
case "$(realpath -m "$out")/" in "$repo/"*) exit 2;; esac
mkdir -p "$out/src" "$out/tests"
fetch() {
 local name=$1 version=$2 url=$3 hash=$4
 local archive="$out/src/$name.tar.xz"
 [[ -f "$archive" ]] || curl -fL --retry 2 --connect-timeout 15 --max-time 180 "$url" -o "$archive"
 echo "$hash  $archive" | sha256sum -c -
 [[ -d "$out/src/$name-$version" ]] || tar xf "$archive" -C "$out/src"
}
fetch mesa 26.2.3 https://archive.mesa3d.org/mesa-26.2.3.tar.xz 1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f
fetch libdrm 2.4.134 https://dri.freedesktop.org/libdrm/libdrm-2.4.134.tar.xz ac5e74d157830eb8bee44c6a6bf3ad49774ef0dd2a72bdad74a8f20308b52a95
meson setup --reconfigure "$out/drm-build" "$out/src/libdrm-2.4.134" --prefix="$out/deps" -Dauto_features=disabled
ninja -C "$out/drm-build" -j4
ninja -C "$out/drm-build" install
export PKG_CONFIG_PATH="$out/deps/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
meson setup --reconfigure "$out/mesa-build" "$out/src/mesa-26.2.3" --prefix="$out/mesa" \
 -Dgallium-drivers=zink -Dvulkan-drivers= -Dplatforms= -Degl=enabled -Dgbm=disabled \
 -Dglx=disabled -Dllvm=disabled -Dvideo-codecs= -Dbuild-tests=false
ninja -C "$out/mesa-build" -j4
ninja -C "$out/mesa-build" install
cc -std=c11 -O2 -g -Wall -Wextra -Werror -I"$out/mesa/include" "$here/zink-offscreen.c" \
 -L"$out/mesa/lib/x86_64-linux-gnu" -Wl,-rpath-link,"$out/mesa/lib/x86_64-linux-gnu:$out/deps/lib/x86_64-linux-gnu" -lEGL -lGLESv2 -o "$out/tests/zink-offscreen"

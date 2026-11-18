#!/usr/bin/env bash
# build-zink.sh: build x86_64 + i386 Zink (GLX/EGL over the Vulkan forwarder) from
# upstream Mesa inside the Flatpak SDK, for the GL.maton extension.
# The subsystem pulls upstream Mesa at MESA_REV and applies its own mesa-*.patch
# to an exported copy. The MatonOS system Mesa fork has nothing to do with this.
# The oldest supported runtime's SDK (25.08) keeps the glibc floor low enough
# for every runtime branch the stub rewrites GL.maton to.
#
# Usage: build-zink.sh [mesa source] [out dir]
# Prints the install trees to pass as MATON_ZINK_STAGE / MATON_ZINK_STAGE_I386.
set -Eeuo pipefail
# Zink builds from pinned upstream Mesa (not MESA_DIR, which is Xwayland's Mesa).
MESA=${1:-${MATON_ZINK_MESA:?set MATON_ZINK_MESA to the upstream Mesa checkout for Zink}}
MESA_URL=https://gitlab.freedesktop.org/mesa/mesa.git
MESA_REV=${MATON_MESA_REV:-b381eaf859621ed7fdb3bfef00ac956e4cca209c}
if [ ! -d "$MESA/.git" ]; then
    git init -q "$MESA" && git -C "$MESA" remote add origin "$MESA_URL"
fi
if ! git -C "$MESA" cat-file -e "$MESA_REV^{commit}" 2>/dev/null; then
    git -C "$MESA" fetch -q --depth 1 origin "$MESA_REV"
fi
OUT=${2:-${MATON_ZINK_OUT:?set MATON_ZINK_OUT to an output directory}}
SDK=${MATON_ZINK_SDK:-org.freedesktop.Sdk//25.08}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/pydeps"
# Patched source copy; re-exported only when MESA_REV or a patch changes,
# so incremental ninja builds keep working.
STAMP="$MESA_REV $(cat "$HERE"/mesa-*.patch | sha256sum | cut -d' ' -f1)"
if [ "$(cat "$OUT/src.stamp" 2>/dev/null)" != "$STAMP" ]; then
    rm -rf "$OUT/src" "$OUT/src.stamp" "$OUT/x86_64" "$OUT/i386"
    mkdir -p "$OUT/src"
    git -C "$MESA" archive "$MESA_REV" | tar -x -C "$OUT/src"
    for p in "$HERE"/mesa-*.patch; do
        patch -d "$OUT/src" -p1 --forward --batch < "$p" || { echo "build-zink: patch failed: $p" >&2; exit 1; }
    done
    echo "$STAMP" > "$OUT/src.stamp"
fi
MESA=$OUT/src
# Mesa's generators need PyYAML; the SDK lacks it. The pure-Python package is enough.
[ -d "$OUT/pydeps/yaml" ] || { cp -r "$(python3 -c 'import yaml,os;print(os.path.dirname(yaml.__file__))')" "$OUT/pydeps/"; rm -f "$OUT"/pydeps/yaml/_yaml*.so; }
OPTS="--libdir=lib --buildtype=release -Db_ndebug=true -Dplatforms=x11,wayland -Dgallium-drivers=zink \
    -Dvulkan-drivers= -Dglx=dri -Degl=enabled -Dglvnd=enabled -Dgbm=disabled -Dllvm=disabled \
    -Dgles1=disabled -Dgles2=enabled -Dvideo-codecs= -Dgallium-va=disabled -Dgallium-rusticl=false \
    -Dtools= -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled"
CROSS=$HERE/zink-i386.cross
rm -rf "$OUT/stage-x86_64" "$OUT/stage-i386"
# Network only for meson's wayland-protocols fallback (SDK copy too old).
# i386: Sdk.Compat.i386 (libraries) + Sdk.Extension.toolchain-i386 (compiler).
flatpak run --share=network --filesystem="$OUT" --filesystem="$CROSS":ro \
    --env=PYTHONPATH="$OUT/pydeps" --command=sh "$SDK" -ec "
cd '$OUT'
[ -f x86_64/build.ninja ] || meson setup x86_64 '$MESA' --prefix=/usr/lib/x86_64-linux-gnu/GL/maton $OPTS
ninja -C x86_64
DESTDIR='$OUT/stage-x86_64' ninja -C x86_64 install >/dev/null
[ -f i386/build.ninja ] || meson setup i386 '$MESA' --cross-file '$CROSS' --prefix=/usr/lib/i386-linux-gnu/GL/maton $OPTS
ninja -C i386
DESTDIR='$OUT/stage-i386' ninja -C i386 install >/dev/null"
echo "MATON_ZINK_STAGE=$OUT/stage-x86_64/usr/lib/x86_64-linux-gnu/GL/maton"
echo "MATON_ZINK_STAGE_I386=$OUT/stage-i386/usr/lib/i386-linux-gnu/GL/maton"

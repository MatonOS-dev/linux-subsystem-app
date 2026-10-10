#!/usr/bin/env bash
# Minimal embedded PipeWire: no ALSA, udev, WirePlumber, D-Bus or hardware access.
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
DEVICE=$(cd "$ROOT/../.." && pwd)
AOSP=${MATON_AOSP_ROOT:-$(cd "$DEVICE/../../.." && pwd)}
NDK=${MATON_NDK:-$HOME/Documents/android-ndk-r30}
BUILD=${MATON_PIPEWIRE_BUILD:-$AOSP/out/matonos/pipewire}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
REV=8fa27cabdc6c0c1350c69c026af5850ef0af1e26 # PipeWire 1.6.9
JOBS=${MATON_BUILD_JOBS:-4}
[[ $JOBS =~ ^[1-4]$ ]] || { echo 'MATON_BUILD_JOBS must be 1..4' >&2; exit 1; }
mkdir -p "$BUILD"
if [[ ! -f $BUILD/src/meson.build ]]; then
    mkdir -p "$BUILD/src"
    if git -C "$AOSP/out/pc-pipewire/src/pipewire" cat-file -e "$REV^{commit}" 2>/dev/null; then
        # git archive deliberately excludes the old hardware build's local patches.
        git -C "$AOSP/out/pc-pipewire/src/pipewire" archive "$REV" | tar -x -C "$BUILD/src"
    else
        git init "$BUILD/upstream"
        git -C "$BUILD/upstream" fetch --depth=1 https://gitlab.freedesktop.org/pipewire/pipewire.git "$REV"
        git -C "$BUILD/upstream" archive "$REV" | tar -x -C "$BUILD/src"
    fi
    printf '%s\n' "$REV" > "$BUILD/src/REVISION"
fi
[[ $(cat "$BUILD/src/REVISION") == "$REV" ]] || { echo 'PipeWire revision mismatch' >&2; exit 1; }
cat > "$BUILD/android.ini" <<CROSS
[binaries]
c = '$TOOLCHAIN/bin/x86_64-linux-android35-clang'
ar = '$TOOLCHAIN/bin/llvm-ar'
strip = '$TOOLCHAIN/bin/llvm-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[built-in options]
c_args = ['-O2', '-march=x86-64-v2', '-I$DEVICE/audio/compat/include', '-include', '$ROOT/compat/android.h']
[properties]
needs_exe_wrapper = true
CROSS
export PKG_CONFIG_LIBDIR="$BUILD/no-host-packages" PKG_CONFIG_PATH=
opts=(--cross-file "$BUILD/android.ini" --prefix /pipewire --libdir lib --buildtype release
    -Dauto_features=disabled -Ddbus=disabled -Dflatpak=disabled -Dspa-plugins=enabled -Dsupport=enabled -Daudioconvert=enabled
    -Dpipewire-jack=disabled -Dpipewire-alsa=disabled -Dpipewire-v4l2=disabled
    -Dsession-managers=[] -Dtests=disabled -Dexamples=disabled)
if [[ -f $BUILD/build/build.ninja ]]; then
    meson setup --reconfigure "$BUILD/build" "$BUILD/src" "${opts[@]}"
else
    meson setup "$BUILD/build" "$BUILD/src" "${opts[@]}"
fi
ninja -C "$BUILD/build" -j"$JOBS"
DESTDIR="$BUILD/install" meson install -C "$BUILD/build" --no-rebuild
mkdir -p "$BUILD/jniLibs/x86_64"
LIBS=$BUILD/install/pipewire/lib
cp -L "$LIBS/libpipewire-0.3.so" "$BUILD/jniLibs/x86_64/"
for module in protocol-native protocol-pulse client-node adapter link-factory spa-node-factory metadata access; do
    cp -L "$LIBS/pipewire-0.3/libpipewire-module-$module.so" "$BUILD/jniLibs/x86_64/"
done
for plugin in support/libspa-support audioconvert/libspa-audioconvert; do
    cp -L "$LIBS/spa-0.2/$plugin.so" "$BUILD/jniLibs/x86_64/"
done
printf 'PipeWire 1.6.9\ncommit %s\nunmodified upstream; NDK compatibility header linux/pipewire/compat/android.h\n' "$REV" > "$BUILD/SOURCE"
cp "$BUILD/src/COPYING" "$BUILD/COPYING"
echo "Embedded PipeWire libraries staged in $BUILD/jniLibs/x86_64"

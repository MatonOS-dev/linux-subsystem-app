#!/usr/bin/env bash
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
AOSP=$(readlink -f "$ROOT/../../../../..")
NDK=${MATON_NDK:-$HOME/Documents/android-ndk-r30}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
API=35
JOBS=${MATON_BUILD_JOBS:-4}
[[ $JOBS =~ ^[1-4]$ ]] || die 'MATON_BUILD_JOBS must be 1..4'
[[ -x $TOOLCHAIN/bin/x86_64-linux-android${API}-clang ]] || die "missing NDK at $NDK"

# Build output lives under out/, never in the source tree: unpacked upstream
# trees carry their own Android.bp files and would clash with AOSP modules.
BUILD=${MATON_COMPOSITOR_BUILD:-$AOSP/out/matonos/compositor}
PREFIX=$BUILD/stage/x86_64
HOST=$BUILD/host
SOURCES=${MATON_COMPOSITOR_SOURCES:-$BUILD/sources}
SRC=$SOURCES/src
mkdir -p "$BUILD" "$PREFIX" "$HOST"
CC=$TOOLCHAIN/bin/x86_64-linux-android${API}-clang
CXX=$TOOLCHAIN/bin/x86_64-linux-android${API}-clang++
AR=$TOOLCHAIN/bin/llvm-ar
RANLIB=$TOOLCHAIN/bin/llvm-ranlib
STRIP=$TOOLCHAIN/bin/llvm-strip

# Android provides POSIX realtime symbols from libc, not librt. This empty
# archive satisfies upstream's unconditional -lrt probe without adding runtime
# code or changing the library sources.
mkdir -p "$PREFIX/lib"
printf 'void maton_empty_rt_archive(void) {}\n' > "$BUILD/empty-rt.c"
"$CC" -fPIC -c "$BUILD/empty-rt.c" -o "$BUILD/empty-rt.o"
"$AR" rcs "$PREFIX/lib/librt.a" "$BUILD/empty-rt.o"
"$CC" -std=c11 -fPIC -c "$ROOT/native/compat_shm.c" -o "$BUILD/compat_shm.o"
"$AR" rcs "$PREFIX/lib/libmaton-shm.a" "$BUILD/compat_shm.o"

for entry in \
  'wayland-1.24.0:7800858844751fc7113d7df3678dc6b58b26a056176a65c49a059763045bffd5' \
  'wayland-protocols-1.48:c563af8e2e784f9599fe23819a3fc5e7d946e76db9f90a2e3ba6c9a869a52911' \
  'pixman-0.46.0:92c1b08db747ff17f9028da3e63168c8ac23204ce62b1a0437607847c8ad6c36' \
  'xkbcommon-1.8.0:025c53032776ed850fbfb92683a703048cd70256df4ac1a1ec41ed3455d5d39c' \
  'libdrm-2.4.134:6b18e4834b0c061232cb5c11e98a6ecdc72ebc6bc282d124406b7a9d4e089ce2' \
  'libffi-3.4.8:bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b' \
  'wlroots-0.20.2:972c7ac44b17828f4702bfae7cd8347346a3fb5b2c1076cfa2c3fcedac5ec343'; do
  name=${entry%%:*}; hash=${entry#*:}
  archive=$SOURCES/downloads/$name.tar.gz
  [[ -s $archive ]] || die "missing pinned source archive $archive"
  echo "$hash  $archive" | sha256sum -c - >/dev/null || die "checksum mismatch: $name"
done

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig:$HOST/lib/pkgconfig:$HOST/lib/x86_64-linux-gnu/pkgconfig:$HOST/share/pkgconfig"

# The Xwayland/X11 stack built by build-xwayland.sh lives in a separate
# prefix; wlroots' xwayland module links against its xcb libraries.
XSTAGE=$BUILD/xstage/x86_64
export PKG_CONFIG_PATH="$XSTAGE/lib/pkgconfig:$XSTAGE/share/pkgconfig:$PKG_CONFIG_PATH"

# MIT-licensed libffi is statically linked into Wayland server.
FFI_SRC=$SRC/libffi-3.4.8
FFI_BUILD=$BUILD/libffi
if [[ ! -f $FFI_BUILD/Makefile ]]; then
  mkdir -p "$FFI_BUILD"
  (cd "$FFI_BUILD" && \
    CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" CFLAGS='-O2 -fPIC' \
    "$FFI_SRC/configure" --host=x86_64-linux-android --prefix="$PREFIX" \
      --enable-static --disable-shared --with-pic --disable-docs)
fi
make -C "$FFI_BUILD" -j"$JOBS"
make -C "$FFI_BUILD" install

cat > "$BUILD/android-x86_64.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
c_args = ['-I$BUILD/generated', '-I$HOST/include', '-Dshm_open=maton_shm_open', '-Dshm_unlink=maton_shm_unlink', '-include', '$ROOT/native/compat_shm.h']
c_link_args = ['-L$PREFIX/lib', '-lmaton-shm']
EOF

# Build the host-only protocol scanner, then install protocol XML data.
# A build directory configured by an older meson cannot be reused: its
# metadata references functions the current meson dropped. Probe with
# --reconfigure and recreate the directory only when that fails.
meson_reuse() { # dir -> 0 when the directory can be reused by this meson
  local dir=$1
  [[ -f $dir/meson-private/build.dat ]] || return 1
  meson setup --reconfigure "$dir" >/dev/null 2>&1
}
if [[ ! -d $BUILD/wayland-host ]] || ! meson_reuse "$BUILD/wayland-host"; then
  rm -rf "$BUILD/wayland-host"
  meson setup "$BUILD/wayland-host" "$SRC/wayland-1.24.0" \
    --prefix="$HOST" --buildtype=release -Dscanner=true -Dlibraries=false \
    -Dtests=false -Ddocumentation=false -Ddtd_validation=false
fi
meson compile -C "$BUILD/wayland-host" -j"$JOBS"
meson install -C "$BUILD/wayland-host"
if ! meson_reuse "$BUILD/protocols-1.48-host"; then
  rm -rf "$BUILD/protocols-1.48-host"
  PATH="$HOST/bin:$PATH" PKG_CONFIG_PATH="$HOST/lib/pkgconfig:$HOST/lib/x86_64-linux-gnu/pkgconfig" \
    meson setup "$BUILD/protocols-1.48-host" "$SRC/wayland-protocols-1.48" \
      --prefix="$HOST" --buildtype=release -Dtests=false
fi
PATH="$HOST/bin:$PATH" PKG_CONFIG_PATH="$HOST/lib/pkgconfig:$HOST/lib/x86_64-linux-gnu/pkgconfig" \
  meson install -C "$BUILD/protocols-1.48-host"
mkdir -p "$PREFIX/include/wayland-protocols"
cp "$HOST/include/wayland-protocols/"*-enum.h "$PREFIX/include/wayland-protocols/"

build_cross() {
  local name=$1 srcname=$2 opts=$3
  local dir=$BUILD/$name
  local source=$SRC/$srcname
  [[ $srcname == wlroots-0.20.2 ]] && source=$SRC/wlroots-0.20.2
  # Compiling a build directory whose metadata was written by an older meson
  # fails late with an opaque regen error, so probe before every compile:
  # reconfigure in place when possible, recreate the directory when not.
  local need_setup=0
  if [[ ! -f $dir/build.ninja || $name == wlroots ]] ||
      ! meson setup --reconfigure "$dir" >/dev/null 2>&1; then
    need_setup=1
  fi
  if [[ $need_setup == 1 ]]; then
    # shellcheck disable=SC2086
    local pkg_libdir="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig:$HOST/lib/pkgconfig:$HOST/lib/x86_64-linux-gnu/pkgconfig:$HOST/share/pkgconfig:$XSTAGE/lib/pkgconfig:$XSTAGE/share/pkgconfig"
    rm -rf "$dir"
    PKG_CONFIG_PATH="$PKG_CONFIG_PATH" PKG_CONFIG_LIBDIR="$pkg_libdir" \
      meson setup "$dir" "$source" --cross-file "$BUILD/android-x86_64.ini" \
        --prefix="$PREFIX" --libdir=lib --buildtype=release $opts
  fi
  PKG_CONFIG_PATH="$PKG_CONFIG_PATH" \
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig:$HOST/lib/pkgconfig:$HOST/lib/x86_64-linux-gnu/pkgconfig:$HOST/share/pkgconfig" \
    meson compile -C "$dir" -j"$JOBS"
  PKG_CONFIG_PATH="$PKG_CONFIG_PATH" \
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig:$HOST/lib/pkgconfig:$HOST/lib/x86_64-linux-gnu/pkgconfig" \
    meson install -C "$dir"
}

build_cross wayland wayland-1.24.0 \
  '-Dscanner=false -Dlibraries=true -Dtests=false -Ddocumentation=false -Ddtd_validation=false'

# Carried wlroots change (to be moved into a proper fork; see UPSTREAMS):
# make the Xwayland socket directory overridable and the abstract-namespace
# bind optional. Android has no writable /tmp, and filesystem permissions are
# the only isolation X11 socket clients can get.
PATCH=$ROOT/wlroots-xwayland-sockets.patch
MARKER='WLR_XWAYLAND_SOCKET_DIR'
if ! grep -q "$MARKER" "$SRC/wlroots-0.20.2/xwayland/sockets.c" 2>/dev/null; then
  (cd "$SRC/wlroots-0.20.2" && patch -p1 --forward < "$PATCH") || die "wlroots patch failed"
fi
build_cross pixman pixman-0.46.0 \
  '-Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dmmx=disabled -Dsse2=enabled -Dssse3=enabled'
mkdir -p "$BUILD/xkb-config"
build_cross xkbcommon xkbcommon-1.8.0 \
  "-Denable-tools=false -Denable-x11=false -Denable-docs=false -Denable-xkbregistry=false -Denable-bash-completion=false -Dxkb-config-root=$BUILD/xkb-config"
build_cross libdrm libdrm-2.4.134 \
  '-Dtests=false -Dinstall-test-programs=false -Dudev=false -Dintel=disabled -Damdgpu=disabled -Dradeon=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled -Dman-pages=disabled'
mkdir -p "$BUILD/generated/wayland-protocols"
"$HOST/bin/wayland-scanner" enum-header \
  "$HOST/share/wayland-protocols/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml" \
  "$BUILD/generated/wayland-protocols/ext-image-copy-capture-v1-enum.h"
"$HOST/bin/wayland-scanner" enum-header \
  "$HOST/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml" \
  "$BUILD/generated/wayland-protocols/xdg-shell-enum.h"
build_cross wlroots wlroots-0.20.2 \
  "-Dbackends=[] -Drenderers=[] -Dallocators=[] -Dexamples=false -Dxwayland=enabled -Dsession=disabled -Dcolor-management=disabled -Dlibliftoff=disabled"

echo "Compositor dependencies installed under $PREFIX"
# Stage runtime .so files the host APK packages (Gradle expects <dir>/<abi>/).
rm -rf "$BUILD/jniLibs"; mkdir -p "$BUILD/jniLibs/x86_64"
find "$PREFIX/lib" -maxdepth 1 -name '*.so' -exec cp -L {} "$BUILD/jniLibs/x86_64/" \;
# The xwayland-enabled wlroots links the xcb stack; bundle the closure into
# the APK (libX11/freetype stay out: only the standalone Xwayland needs them).
for lib in libxcb libxcb-composite libxcb-ewmh libxcb-icccm libxcb-render \
           libxcb-res libxcb-xfixes libxcb-errors libXau libXdmcp; do
  candidate=$XSTAGE/lib/$lib.so
  [[ -f $candidate ]] || die "missing $candidate; run build-xwayland.sh"
  cp -L "$candidate" "$BUILD/jniLibs/x86_64/"
done
echo "APK native libraries staged under $BUILD/jniLibs/x86_64"

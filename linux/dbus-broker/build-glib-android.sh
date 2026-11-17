#!/usr/bin/env bash
# Build the broker's minimal static bionic GLib dependency set from the
# repository's locked and already-fetched Flatpak sources.
set -Eeuo pipefail

DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
NDK=${MATON_NDK:?set MATON_NDK (see DEPENDENCIES.md in the device tree)}
OUT=${MATON_SUBSYSTEM_OUT:?set MATON_SUBSYSTEM_OUT}
PREFIX=${MATON_BROKER_GLIB_PREFIX:-$OUT/deps/broker-glib}
CACHE=${FLATPAK_CACHE:?set FLATPAK_CACHE to the fetched Flatpak source cache}
SOURCE=$CACHE/src
BUILD=$OUT/deps/broker-glib-build
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$TOOLCHAIN/bin/x86_64-linux-android35-clang
AR=$TOOLCHAIN/bin/llvm-ar
RANLIB=$TOOLCHAIN/bin/llvm-ranlib
STRIP=$TOOLCHAIN/bin/llvm-strip
CMAKE=${CMAKE:-cmake}
COMMON_FLAGS='-O2 -march=x86-64-v2 -D_GNU_SOURCE -fPIC'
JOBS=${MATON_BUILD_JOBS:-$(nproc)}
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || { echo 'MATON_BUILD_JOBS must be a positive integer' >&2; exit 1; }
for tool in "$CC" "$AR" "$RANLIB" "$STRIP" meson ninja "$CMAKE"; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing required tool: $tool" >&2; exit 1; }
done

LOCK=$DEVICE_DIR/linux/flatpak/build-support/sources.lock
pin_for() { awk -F'|' -v name="$1" '$1 == name { print $4; found=1 } END { if (!found) exit 1 }' "$LOCK"; }
for name in glib pcre2 libffi glib-proxy-libintl; do
  [[ -s $CACHE/src/$name.pin ]] || { echo "Missing fetched source pin: $CACHE/src/$name.pin (run fetch.sh first)" >&2; exit 1; }
  expected=$(pin_for "$name")
  actual=$(cut -f2 "$CACHE/src/$name.pin")
  [[ $actual == "$expected" ]] || { echo "Source pin mismatch for $name: $actual != $expected" >&2; exit 1; }
done
for path in "$SOURCE/glib/meson.build" "$SOURCE/pcre2/CMakeLists.txt" "$SOURCE/libffi/configure" \
    "$SOURCE/glib/subprojects/packagecache/proxy-libintl-0.5.tar.gz"; do
  [[ -e $path ]] || { echo "Missing fetched broker source input: $path (run fetch.sh first)" >&2; exit 1; }
done

mkdir -p "$OUT/deps" "$BUILD"
STAMP=$({ printf '%s\n' "$(pin_for glib)" "$(pin_for pcre2)" "$(pin_for libffi)" \
    "$(pin_for glib-proxy-libintl)" 'ndk-api=35' "$COMMON_FLAGS"; sha256sum "${BASH_SOURCE[0]}"; \
    "$CC" --version | sed -n '1p'; } | sha256sum | cut -d' ' -f1)
if [[ -f $PREFIX/.source-stamp && $(cat "$PREFIX/.source-stamp") == "$STAMP" && \
      -s $PREFIX/lib/libgio-2.0.a && -s $PREFIX/lib/libgobject-2.0.a && \
      -s $PREFIX/lib/libgmodule-2.0.a && -s $PREFIX/lib/libglib-2.0.a && \
      -s $PREFIX/lib/libintl.a && -s $PREFIX/lib/libpcre2-8.a && -s $PREFIX/lib/libffi.a ]]; then
  echo "Broker GLib dependencies are up to date in $PREFIX"
  exit 0
fi
mkdir -p "$PREFIX"
HOST=x86_64-linux-android
export CC AR RANLIB STRIP CFLAGS=$COMMON_FLAGS CPPFLAGS='-D_GNU_SOURCE' LDFLAGS=''

# libffi is built first so GLib's Meson checks can discover it via pkg-config.
mkdir -p "$BUILD/libffi"
cd "$BUILD/libffi"
"$SOURCE/libffi/configure" --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static \
  --disable-docs --disable-multi-os-directory CFLAGS="$COMMON_FLAGS"
make -j"$JOBS"
make install

rm -rf "$BUILD/pcre2"
"$CMAKE" -S "$SOURCE/pcre2" -B "$BUILD/pcre2" -G Ninja \
  -DCMAKE_SYSTEM_NAME=Android -DCMAKE_ANDROID_NDK="$NDK" -DCMAKE_ANDROID_ARCH_ABI=x86_64 \
  -DCMAKE_ANDROID_API=35 -DCMAKE_C_COMPILER="$CC" -DCMAKE_AR="$AR" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$COMMON_FLAGS" -DCMAKE_INSTALL_PREFIX:PATH="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF \
  -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF
"$CMAKE" --build "$BUILD/pcre2" --target install --parallel "$JOBS"

cat > "$BUILD/android.ini" <<CROSS
[binaries]
c = '$CC'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'pkg-config'
[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[built-in options]
c_args = ['-O2', '-march=x86-64-v2', '-D_GNU_SOURCE', '-fPIC', '-I$PREFIX/include']
c_link_args = ['-landroid', '-llog']
[properties]
needs_exe_wrapper = true
CROSS
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig PKG_CONFIG_PATH=
if [[ -d $BUILD/glib && ! -f $BUILD/glib/build.ninja ]]; then rm -rf "$BUILD/glib"; fi
meson setup "$BUILD/glib" "$SOURCE/glib" --cross-file "$BUILD/android.ini" \
  --prefix "$PREFIX" --libdir lib --default-library static --prefer-static --buildtype release \
  -Dtests=false -Dman-pages=disabled -Dintrospection=disabled -Ddocumentation=false \
  -Dselinux=disabled -Dlibmount=disabled -Dsystemtap=disabled -Dforce_posix_threads=true
ninja -C "$BUILD/glib" -j"$JOBS" install

for lib in libgio-2.0.a libgobject-2.0.a libgmodule-2.0.a libglib-2.0.a libintl.a libpcre2-8.a libffi.a; do
  [[ -s $PREFIX/lib/$lib ]] || { echo "Broker dependency build did not produce $PREFIX/lib/$lib" >&2; exit 1; }
done
printf '%s\n' "$STAMP" > "$PREFIX/.source-stamp"
echo "Built static Android broker GLib dependencies in $PREFIX"

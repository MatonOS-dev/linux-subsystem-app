#!/usr/bin/env bash
# Build the broker's pinned static GLib dependency prefix for Android/bionic.
set -Eeuo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
NDK=${MATON_NDK:?set MATON_NDK}
OUT=${MATON_SUBSYSTEM_OUT:?set MATON_SUBSYSTEM_OUT}
PREFIX=${MATON_BROKER_GLIB_PREFIX:?set MATON_BROKER_GLIB_PREFIX}
CACHE=${MATON_BROKER_GLIB_CACHE:-${MATON_FLATPAK_CACHE:-${FLATPAK_CACHE:-}}}
[[ -n $CACHE ]] || { echo 'Set MATON_BROKER_GLIB_CACHE (or MATON_FLATPAK_CACHE) to the source cache directory' >&2; exit 2; }
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$TOOLCHAIN/bin/x86_64-linux-android35-clang
AR=$TOOLCHAIN/bin/llvm-ar
RANLIB=$TOOLCHAIN/bin/llvm-ranlib
STRIP=$TOOLCHAIN/bin/llvm-strip
CMAKE=${CMAKE:-cmake}
JOBS=${MATON_BUILD_JOBS:-$(nproc)}
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || { echo 'MATON_BUILD_JOBS must be a positive integer' >&2; exit 2; }
for tool in "$CC" "$AR" "$RANLIB" "$STRIP" meson ninja "$CMAKE" make curl sha256sum; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing required tool: $tool" >&2; exit 1; }
done

LOCK=$ROOT/linux/dbus-broker/sources.lock
TARBALLS=$CACHE/tar
SOURCES=$CACHE/src/dbus-broker
BUILD=$OUT/deps/broker-glib-build
mkdir -p "$TARBALLS" "$SOURCES" "$BUILD"

while IFS='|' read -r name url version digest archive_name; do
  [[ -n $name && $name != \#* ]] || continue
  archive=$TARBALLS/$archive_name
  if [[ ! -s $archive ]]; then
    temporary=$archive.tmp.$$
    trap 'rm -f "$temporary"' EXIT
    curl -fL --retry 2 "$url" -o "$temporary"
    mv "$temporary" "$archive"
    trap - EXIT
  fi
  printf '%s  %s\n' "$digest" "$archive" | sha256sum -c -
  source=$SOURCES/$name-$version
  rm -rf "$source"
  mkdir -p "$source"
  tar -xf "$archive" -C "$source" --strip-components=1
done < "$LOCK"

GLIB=$SOURCES/glib-2.90.0
PCRE2=$SOURCES/pcre2-10.46
LIBFFI=$SOURCES/libffi-3.5.2
PROXY_INTL=$TARBALLS/proxy-libintl-0.5.tar.gz
COMMON_FLAGS='-O2 -march=x86-64-v2 -D_GNU_SOURCE -fPIC'
HOST=x86_64-linux-android
export CC AR RANLIB STRIP CFLAGS=$COMMON_FLAGS CPPFLAGS=-D_GNU_SOURCE LDFLAGS=

STAMP=$({ sha256sum "$LOCK" "${BASH_SOURCE[0]}"; printf '%s\n' "$HOST" android-api=35 "$COMMON_FLAGS"; "$CC" --version | sed -n '1p'; } | sha256sum | cut -d' ' -f1)
if [[ -f $PREFIX/.source-stamp && $(cat "$PREFIX/.source-stamp") == "$STAMP" && \
      -s $PREFIX/lib/libgio-2.0.a && -s $PREFIX/lib/libgobject-2.0.a && \
      -s $PREFIX/lib/libgmodule-2.0.a && -s $PREFIX/lib/libglib-2.0.a && \
      -s $PREFIX/lib/libintl.a && -s $PREFIX/lib/libpcre2-8.a && -s $PREFIX/lib/libffi.a ]]; then
  echo "Broker GLib dependencies are up to date in $PREFIX"
  exit 0
fi

mkdir -p "$PREFIX"
rm -rf "$BUILD/libffi"
mkdir -p "$BUILD/libffi"
cd "$BUILD/libffi"
"$LIBFFI/configure" --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static \
  --disable-docs --disable-multi-os-directory CFLAGS="$COMMON_FLAGS"
make -j"$JOBS"
make install

rm -rf "$BUILD/pcre2"
"$CMAKE" -S "$PCRE2" -B "$BUILD/pcre2" -G Ninja \
  -DCMAKE_SYSTEM_NAME=Android -DCMAKE_ANDROID_NDK="$NDK" -DCMAKE_ANDROID_ARCH_ABI=x86_64 \
  -DCMAKE_ANDROID_API=35 -DCMAKE_C_COMPILER="$CC" -DCMAKE_AR="$AR" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$COMMON_FLAGS" -DCMAKE_INSTALL_PREFIX:PATH="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF \
  -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF
"$CMAKE" --build "$BUILD/pcre2" --target install --parallel "$JOBS"

mkdir -p "$GLIB/subprojects/packagecache"
install -m644 "$PROXY_INTL" "$GLIB/subprojects/packagecache/proxy-libintl-0.5.tar.gz"
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
rm -rf "$BUILD/glib"
meson setup "$BUILD/glib" "$GLIB" --cross-file "$BUILD/android.ini" \
  --prefix "$PREFIX" --libdir lib --default-library static --prefer-static --buildtype release \
  --wrap-mode=nodownload \
  -Dtests=false -Dman-pages=disabled -Dintrospection=disabled -Ddocumentation=false \
  -Dselinux=disabled -Dlibmount=disabled -Dsystemtap=disabled -Dsysprof=disabled \
  -Dforce_posix_threads=true
ninja -C "$BUILD/glib" -j"$JOBS" install

for lib in libgio-2.0.a libgobject-2.0.a libgmodule-2.0.a libglib-2.0.a libintl.a libpcre2-8.a libffi.a; do
  [[ -s $PREFIX/lib/$lib ]] || { echo "Broker dependency build did not produce $PREFIX/lib/$lib" >&2; exit 1; }
done
printf '%s\n' "$STAMP" > "$PREFIX/.source-stamp"
echo "Built pinned static Android broker GLib dependencies in $PREFIX"

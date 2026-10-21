#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CACHE=${CACHE_DIR:-$HERE/cache}
SRC=$CACHE/src
BUILD=${BUILD_DIR:-$HERE/build}
# Runtime paths must never name the staging tree. Keep build/prefix for APK
# consumers; DESTDIR supplies the filesystem root used only while installing.
INSTALL_PREFIX=/usr
PREFIX=$BUILD/prefix
DESTDIR=$BUILD/stage
export DESTDIR
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1767225600}
export ZERO_AR_DATE=1
export ARFLAGS=crD
export LC_ALL=C TZ=UTC
OUT=${OUT_DIR:-$HERE/out}
JOBS=${JOBS:-4}
[ "$JOBS" -le 4 ] || { echo 'JOBS must be <= 4' >&2; exit 2; }
MUSL_PREFIX=${MUSL_PREFIX:-/usr}
CC=${CC:-$MUSL_PREFIX/bin/x86_64-alpine-linux-musl-gcc}
CXX=${CXX:-$MUSL_PREFIX/bin/x86_64-alpine-linux-musl-g++}
AR=${AR:-$MUSL_PREFIX/bin/ar}
STRIP=${STRIP:-$MUSL_PREFIX/bin/strip}
READELF=${READELF:-$MUSL_PREFIX/bin/readelf}
RANLIB=${RANLIB:-$MUSL_PREFIX/bin/ranlib}
OBJCOPY=${OBJCOPY:-$MUSL_PREFIX/bin/objcopy}
HOST=x86_64-linux-musl
MESON=${MESON:-$SRC/meson/meson.py}
CMAKE=${CMAKE:-$(command -v cmake || true)}
NINJA=${NINJA:-$(command -v ninja || true)}
[ -x "$CMAKE" ] || CMAKE=$(command -v cmake || true)
[ -x "$NINJA" ] || NINJA=$(command -v ninja || true)
for exe in "$CC" "$CXX" "$AR" "$STRIP" "$READELF" "$CMAKE" "$NINJA"; do
  [ -x "$exe" ] || { echo "Missing build tool: $exe" >&2; exit 1; }
done
[ -f "$MESON" ] || { echo "Missing Meson module: $MESON" >&2; exit 1; }
mkdir -p "$BUILD" "$PREFIX" "$OUT" "$DESTDIR"
ln -sfn ../prefix "$DESTDIR/usr"
rm -f "$OUT/flatpak" "$OUT/ostree" "$OUT/bwrap" \
  "$OUT/flatpak.unstripped" "$OUT/ostree.unstripped" "$OUT/bwrap.unstripped" \
  "$OUT/matonos-flatpak" "$OUT/matonos-flatpak.unstripped" \
  "$OUT/matonos-flatpak-store" "$OUT/SOURCE"
export PATH="$(dirname "$NINJA"):$PATH"
export CC CXX AR RANLIB STRIP
# Map each independently located input, including generated/copied sources.
MESON_SRC=$(python3 -c 'import os, sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' "$SRC" "$BUILD/component")
REPRO_FLAGS="-ffile-prefix-map=$MESON_SRC=./src -fdebug-prefix-map=$MESON_SRC=./src -ffile-prefix-map=$SRC=./src -fdebug-prefix-map=$SRC=./src -ffile-prefix-map=$BUILD=./build -fdebug-prefix-map=$BUILD=./build -ffile-prefix-map=$HERE=./build-support -fdebug-prefix-map=$HERE=./build-support -ffile-prefix-map=$MUSL_PREFIX=./musl -fdebug-prefix-map=$MUSL_PREFIX=./musl -ffile-prefix-map=/home/buildozer=./alpine-build -fdebug-prefix-map=/home/buildozer=./alpine-build"
export CFLAGS="-O2 -fPIC $REPRO_FLAGS"
export CXXFLAGS="$CFLAGS"
COMPAT_CFLAGS="$CFLAGS"
COMPAT_CXXFLAGS="$CXXFLAGS"
export CPPFLAGS="-I$SRC/e2fsprogs-headers/usr/include"
export LDFLAGS="-L$PREFIX/lib"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
export PKG_CONFIG_PATH=
PKG_CONFIG_STATIC="$BUILD/pkg-config-static"
cat > "$PKG_CONFIG_STATIC" <<EOF
#!/bin/sh
exec /usr/bin/pkg-config --define-variable=prefix="$PREFIX" --static "\$@"
EOF
chmod +x "$PKG_CONFIG_STATIC"
export PKG_CONFIG="$PKG_CONFIG_STATIC"
export PATH="$PREFIX/bin:$PATH"
export PATH="$SRC/gperf-host-tool/usr/bin:$PATH"
cat > "$BUILD/musl.ini" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = '$PKG_CONFIG_STATIC'
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
EOF
mkdir -p "$PREFIX/lib/pkgconfig"
cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=$MUSL_PREFIX/include
Name: zlib
Description: Alpine musl zlib
Version: 1.3.1
Libs: -lz
Cflags: -I\${includedir}
EOF
cat > "$PREFIX/lib/pkgconfig/e2p.pc" <<EOF
prefix=$PREFIX
includedir=$SRC/e2fsprogs-headers/usr/include
Name: e2p
Description: e2fsprogs header declarations used by OSTree
Version: 1.47.0
Libs:
Cflags: -I\${includedir}
EOF
stage_metadata() {
  # pkg-config overrides the install prefix when consuming our staged .pc
  # files. Preserve /usr/lib references to Alpine's musl toolchain archives.
  :
}
meson() { python3 "$MESON" "$@"; }
MCOMMON="--cross-file $BUILD/musl.ini --prefix $INSTALL_PREFIX --sysconfdir /etc --localstatedir /var --libdir lib --default-library static --prefer-static"

START_AT=${START_AT:-all}
if [ "$START_AT" = all ]; then
mkdir -p "$BUILD/libffi"; cd "$BUILD/libffi"
"$SRC/libffi/configure" --host="$HOST" --prefix="$INSTALL_PREFIX" --sysconfdir=/etc --localstatedir=/var --disable-shared --enable-static CFLAGS="$CFLAGS"
make -j"$JOBS"; make install; stage_metadata
"$CMAKE" -S "$SRC/pcre2" -B "$BUILD/pcre2" -G Ninja -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF
"$CMAKE" --build "$BUILD/pcre2" --target install --parallel "$JOBS"; stage_metadata
meson setup "$BUILD/glib" "$SRC/glib" $MCOMMON -Dtests=false -Dman-pages=disabled -Dintrospection=disabled -Ddocumentation=false -Dselinux=disabled
"$NINJA" -C "$BUILD/glib" -j "$JOBS" install; stage_metadata
"$CMAKE" -S "$SRC/zstd/build/cmake" -B "$BUILD/zstd" -G Ninja -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_SHARED=OFF
"$CMAKE" --build "$BUILD/zstd" --target install --parallel "$JOBS"; stage_metadata
meson setup "$BUILD/libxml2" "$SRC/libxml2" $MCOMMON -Ddocs=disabled -Dpython=disabled -Dminimum=false -Doutput=enabled -Dzlib=enabled
"$NINJA" -C "$BUILD/libxml2" -j "$JOBS" install; stage_metadata
meson setup "$BUILD/json-glib" "$SRC/json-glib" $MCOMMON -Dtests=false -Dman=false -Dintrospection=disabled
"$NINJA" -C "$BUILD/json-glib" -j "$JOBS" install; stage_metadata
meson setup "$BUILD/libxmlb" "$SRC/libxmlb" $MCOMMON -Dtests=false -Dcli=false -Dintrospection=false -Dgtkdoc=false
"$NINJA" -C "$BUILD/libxmlb" -j "$JOBS" install; stage_metadata
"$CMAKE" -S "$SRC/boringssl" -B "$BUILD/boringssl" -G Ninja -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -DBUILD_TESTING=OFF -DOPENSSL_NO_ASM=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
"$CMAKE" --build "$BUILD/boringssl" --parallel "$JOBS" --target crypto ssl
mkdir -p "$PREFIX/lib" "$PREFIX/include/openssl"
cp "$BUILD/boringssl/libcrypto.a" "$PREFIX/lib/"; cp "$BUILD/boringssl/libssl.a" "$PREFIX/lib/"
cp -R "$SRC/boringssl/include/openssl/." "$PREFIX/include/openssl/"
mkdir -p "$BUILD/boringssl-headers/src"
ln -sfn "$SRC/boringssl/include" "$BUILD/boringssl-headers/src/include"
if [ ! -d "$BUILD/gpgme-lite-src" ]; then cp -R "$SRC/DullPGP" "$BUILD/gpgme-lite-src"; fi
(cd "$BUILD/gpgme-lite-src" && make clean && make -j"$JOBS" CC="$CC" AR="$AR" CFLAGS="$CFLAGS" BSSL_SRC="$BUILD/boringssl-headers" BSSL_LIBDIR="$PREFIX/lib" PREFIX="$INSTALL_PREFIX" LIBDIR="$INSTALL_PREFIX/lib" INCLUDEDIR="$INSTALL_PREFIX/include/gpgme-lite")
install -D -m644 "$BUILD/gpgme-lite-src/libgpgme-lite.a" "$PREFIX/lib/libgpgme-lite.a"
install -d "$PREFIX/include/gpgme-lite"; cp "$SRC/DullPGP/include/"*.h "$PREFIX/include/gpgme-lite/"
cat > "$PREFIX/lib/pkgconfig/gpgme.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include/gpgme-lite
Name: gpgme-lite
Description: DullPGP gpgme-lite
Version: 2.0.1
Libs: -L\${libdir} -lgpgme-lite
Libs.private: -L\${libdir} -lcrypto
Cflags: -I\${includedir}
EOF
cat > "$PREFIX/lib/pkgconfig/gpg-error.pc" <<EOF
prefix=$PREFIX
includedir=\${prefix}/include/gpgme-lite
Name: gpg-error
Description: gpgme-lite error compatibility
Version: 1.0
Libs:
Cflags: -I\${includedir}
EOF

mkdir -p "$BUILD/libseccomp"; cd "$BUILD/libseccomp"
"$SRC/libseccomp/configure" --host="$HOST" --prefix="$INSTALL_PREFIX" --sysconfdir=/etc --localstatedir=/var --disable-shared --enable-static --disable-python --disable-tests CFLAGS="$CFLAGS"
make -j"$JOBS"; make install; stage_metadata
if [ ! -d "$BUILD/libcap-src" ]; then cp -R "$SRC/libcap" "$BUILD/libcap-src"; fi
make -C "$BUILD/libcap-src/libcap" -j"$JOBS" CC="$CC" AR="$AR" RANLIB="$RANLIB" prefix="$INSTALL_PREFIX" lib=lib LIBDIR="$INSTALL_PREFIX/lib" SHARED=no PTHREADS=no BUILD_CC=cc
make -C "$BUILD/libcap-src/libcap" install-static FAKEROOT="$DESTDIR" CC="$CC" AR="$AR" RANLIB="$RANLIB" prefix="$INSTALL_PREFIX" lib=lib LIBDIR="$INSTALL_PREFIX/lib" SHARED=no PTHREADS=no BUILD_CC=cc; stage_metadata
meson setup "$BUILD/bubblewrap" "$SRC/bubblewrap" $MCOMMON -Dtests=false -Dman=disabled -Dc_args="$COMPAT_CFLAGS"
"$NINJA" -C "$BUILD/bubblewrap" -j "$JOBS"

mkdir -p "$BUILD/libarchive"; cd "$BUILD/libarchive"
"$SRC/libarchive/configure" --host="$HOST" --prefix="$INSTALL_PREFIX" --sysconfdir=/etc --localstatedir=/var --disable-shared --enable-static --without-openssl --without-nettle --without-mbedtls --without-lzo2 --without-bz2lib --without-lz4 --without-zstd --without-xml2 CPPFLAGS="$CPPFLAGS" CFLAGS="$CFLAGS"
make -j"$JOBS"; make install; stage_metadata
mkdir -p "$BUILD/curl"; cd "$BUILD/curl"
"$SRC/curl/configure" --host="$HOST" --prefix="$INSTALL_PREFIX" --sysconfdir=/etc --localstatedir=/var --disable-shared --enable-static --with-ssl="$PREFIX" --with-zlib="$MUSL_PREFIX" --with-ca-path=/apex/com.android.conscrypt/cacerts --without-ca-bundle --without-ca-fallback --disable-ftp --enable-file --disable-websockets --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt --without-libssh2 --without-nghttp2 --without-libidn2 --without-libpsl --without-brotli CFLAGS="$CFLAGS" CPPFLAGS="$CPPFLAGS" LDFLAGS="$LDFLAGS -lstdc++ -pthread" LIBS='-lstdc++ -pthread'
make -j"$JOBS"; make install; stage_metadata
# BoringSSL's static archives contain C++ objects. Keep -lstdc++ in curl's
# static dependency metadata so C-linker consumers resolve those objects.
"$CMAKE" -S "$SRC/liblzma" -B "$BUILD/liblzma" -G Ninja -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -DBUILD_SHARED_LIBS=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_DOC=OFF -DXZ_NLS=OFF
"$CMAKE" --build "$BUILD/liblzma" --target install --parallel "$JOBS"; stage_metadata
fi

if [ "$START_AT" != flatpak ]; then
mkdir -p "$BUILD/ostree"; cd "$BUILD/ostree"
if [ ! -x "$SRC/ostree/configure" ]; then
  (cd "$SRC/ostree" && ACLOCAL_PATH="$SRC/DullPGP" NOCONFIGURE=1 ./autogen.sh)
fi
ac_cv_c_undeclared_builtin_options='none needed' "$SRC/ostree/configure" --host="$HOST" --prefix="$INSTALL_PREFIX" --sysconfdir=/etc --localstatedir=/var --libdir="$INSTALL_PREFIX/lib" --disable-shared --enable-static --with-curl --with-gpgme --without-soup --without-soup3 --without-selinux --without-libmount --without-avahi --disable-rofiles-fuse --disable-man --disable-installed-tests --without-composefs --with-crypto=glib CC="$CC" CFLAGS="$COMPAT_CFLAGS -I$PREFIX/include/gpgme-lite" LDFLAGS="$LDFLAGS" LIBS="-lstdc++ -pthread" PKG_CONFIG="$PKG_CONFIG" PKG_CONFIG_LIBDIR="$PKG_CONFIG_LIBDIR"
printf '#include "config.h"\n' > "$BUILD/ostree/libglnx-config.h"
make -j"$JOBS" ostree
make install-libLTLIBRARIES install-libostreeincludeHEADERS install-pkgconfigDATA; stage_metadata
# libtool may fold dependency archives into a static convenience archive.
# Keep only relocatable object members in the public libostree archive.
OSTREE_ARCHIVE="$PREFIX/lib/libostree-1.a"
if [ -f "$OSTREE_ARCHIVE" ]; then
  mkdir -p "$BUILD/libostree-repack"
  rm -f "$BUILD/libostree-repack"/*
  (cd "$BUILD/libostree-repack"; "$AR" t "$OSTREE_ARCHIVE" | while IFS= read -r member; do
    case "$member" in
      la-glnx-*.o) ;; # Flatpak's selected libglnx archive supplies these symbols.
      *.o) "$AR" x "$OSTREE_ARCHIVE" "$member";;
    esac
  done; "$AR" qc libostree-1.a ./*.o; "$RANLIB" libostree-1.a)
  mv "$BUILD/libostree-repack/libostree-1.a" "$OSTREE_ARCHIVE"
fi

fi

FLATPAK_CFLAGS="$COMPAT_CFLAGS"
FLATPAK_CXXFLAGS="$COMPAT_CXXFLAGS"
# system_dbus_proxy names a path that is deliberately NOT shipped: since v1 the
# stub-hosted per-app bus (dbus-java + broker) filters D-Bus, so no app uses
# xdg-dbus-proxy. Leaving the option unset would build Flatpak's bundled copy.
meson setup "$BUILD/flatpak" "$SRC/flatpak" $MCOMMON -Dtests=false -Dinstalled_tests=false -Ddocbook_docs=disabled -Dgtkdoc=disabled -Dman=disabled -Dgir=disabled -Ddconf=disabled -Dsystemd=disabled -Dsystem_helper=disabled -Dmalcontent=disabled -Dselinux_module=disabled -Dauto_sideloading=false -Dxauth=disabled -Dsystem_bubblewrap=/apex/com.matonos.flatpak/bin/matonos-app-bwrap -Dsystem_dbus_proxy=/apex/com.matonos.flatpak/bin/xdg-dbus-proxy -Dc_args="$FLATPAK_CFLAGS" -Dcpp_args="$FLATPAK_CXXFLAGS" -Dc_link_args="-static-pie -static" -Dcpp_link_args="-static-pie -static"
"$NINJA" -C "$BUILD/flatpak" -j "$JOBS"
python3 "$HERE/make-multicall.py" "$BUILD" "$OUT/matonos-flatpak.unstripped" "$OBJCOPY" "$CC"
"$STRIP" --strip-all "$OUT/matonos-flatpak.unstripped" -o "$OUT/matonos-flatpak"
rm -f "$OUT/matonos-flatpak.unstripped"
if "$READELF" -l "$OUT/matonos-flatpak" | grep -q 'INTERP'; then
  echo 'Static musl Flatpak multicall has PT_INTERP' >&2
  exit 1
fi

if strings "$OUT/matonos-flatpak" | LC_ALL=C grep -E '/home/|/mnt/' | grep -v '^/home/buildozer/aports/main/gcc/src/build/'; then
  echo 'Unexpected host path leaked into matonos-flatpak' >&2
  exit 1
fi
python3 "$HERE/license-gate.py"
{
  echo 'flatpak-musl source manifest'; cat "$HERE/sources.lock"; echo
  echo "toolchain: Alpine 3.24.2; $($CC --version | head -1); musl 1.2.6-r2"
  echo
  for bin in matonos-flatpak; do (cd "$OUT" && sha256sum "$bin"); done
} > "$OUT/SOURCE"
echo "Artifacts and provenance written to $OUT"

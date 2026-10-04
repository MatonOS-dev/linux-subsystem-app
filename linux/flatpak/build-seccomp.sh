#!/usr/bin/env bash
# Rebuild only the Flatpak CLI from the checked-out bionic fork, using the
# existing NDK dependency prefix and the image's already packaged libseccomp.
set -euo pipefail
DEVICE=$(cd -- "$(dirname -- "$0")/../.." && pwd)
: "${MATON_AOSP:?Set MATON_AOSP to the matching AOSP checkout}"
NDK=${ANDROID_NDK:-$MATON_AOSP/out/matonos/flatpak-ndk/android-ndk-r30}
PREFIX=${MATON_FLATPAK_PREFIX:-$MATON_AOSP/out/matonos/flatpak-ndk/prefix}
SOURCE=${MATON_FLATPAK_SOURCE:-$MATON_AOSP/device/maton/pc_x86_64/linux/third_party/flatpak/upstream}
SECCOMP_SOURCE=${MATON_SECCOMP_SOURCE:-$MATON_AOSP/device/maton/pc_x86_64/linux/third_party/libseccomp/upstream}
PRODUCT=${MATON_PRODUCT_OUT:-$MATON_AOSP/out/target/product/pc_x86_64}
BUILD=${MATON_FLATPAK_BUILD:-$DEVICE/linux/flatpak/build/seccomp}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[[ -f $PRODUCT/system_ext/lib64/libseccomp_matonos.so && -f $SECCOMP_SOURCE/include/seccomp.h ]]
mkdir -p "$BUILD"
BUILD=$(cd "$BUILD" && pwd)
# configure/gettext can generate files in the source directory: use a private copy.
[[ ! -e $BUILD/source ]] || { echo 'Use a fresh MATON_FLATPAK_BUILD directory' >&2; exit 1; }
mkdir "$BUILD/source"
tar -C "$SOURCE" --exclude=./.git -cf - . | tar -C "$BUILD/source" -xf -
mkdir -p "$BUILD/obj"
cd "$BUILD/obj"
export CC=$TC/x86_64-linux-android35-clang
export CFLAGS='-O2 -fPIC -march=x86-64-v2'
export CPPFLAGS="-I$MATON_AOSP/external/libcap/libcap/include"
export LDFLAGS="-L$PRODUCT/system/lib64"
export PKG_CONFIG_PATH=$PREFIX/lib64/pkgconfig
export PKG_CONFIG_LIBDIR=$PREFIX/lib64/pkgconfig
export LIBSECCOMP_CFLAGS="-I$SECCOMP_SOURCE/include"
export LIBSECCOMP_LIBS="-L$PRODUCT/system_ext/lib64 -lseccomp_matonos"
# configure executes its bwrap version probe on the host, never the target ELF.
cat > "$BUILD/bwrap-probe" <<'PROBE'
#!/bin/sh
if [ "$1" = --version ]; then echo 'bubblewrap 0.10.0'; else echo 'usage: bwrap --bind-fd FD'; fi
PROBE
chmod +x "$BUILD/bwrap-probe"
"$BUILD/source/configure" --host=x86_64-linux-android --prefix="$PREFIX" \
    --libdir="$PREFIX/lib64" --disable-maintainer-mode --disable-documentation \
    --disable-gtk-doc-check --disable-docbook-docs --disable-system-helper \
    --with-systemd=no --with-curl --disable-xauth --disable-selinux-module \
    --enable-seccomp --with-priv-mode=none --with-system-dbus-proxy=no \
    BWRAP="$BUILD/bwrap-probe"
rg -q '^#define ENABLE_SECCOMP 1$' config.h
python3 "$DEVICE/linux/flatpak/tests/seccomp-syscalls-test.py" "$NDK" "$BUILD/source" "$SECCOMP_SOURCE"
printf '%s\n' 'maton-built-sources: $(BUILT_SOURCES)' > "$BUILD/built-sources.mk"
make -j"${MATON_BUILD_JOBS:-2}" -f Makefile -f "$BUILD/built-sources.mk" maton-built-sources
make -j"${MATON_BUILD_JOBS:-2}" flatpak
"$TC/llvm-readelf" -d flatpak | rg 'Shared library: \[libseccomp_matonos.so\]'
"$TC/llvm-nm" -D flatpak | rg ' U seccomp_export_bpf$'
"$TC/llvm-strings" flatpak | rg '^--seccomp$'
"$TC/llvm-strip" --strip-unneeded -o "$BUILD/matonos-flatpak" flatpak
install -m 0755 "$BUILD/matonos-flatpak" "$DEVICE/linux/flatpak/prebuilt/system_ext/bin/matonos-flatpak"
rg '^#define ENABLE_SECCOMP 1$' config.h > "$DEVICE/linux/flatpak/seccomp-config.h"
(cd "$DEVICE/linux/flatpak/prebuilt/system_ext" && sha256sum bin/matonos-flatpak) > "$DEVICE/linux/flatpak/seccomp.sha256"

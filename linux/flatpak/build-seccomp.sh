#!/usr/bin/env bash
# Build the complete consumer Flatpak fork with seccomp, then stage
# after all checks pass. Dependencies are read from the matching AOSP/NDK build.
#
# r24: the Flatpak stack ships in the updatable com.matonos.flatpak APEX and is
# compiled with that APEX as its prefix, so its compiled-in FLATPAK_BINDIR,
# LIBEXECDIR, FLATPAK_DATADIR, HELPER (bwrap) and DBUSPROXY paths all point at
# /apex/com.matonos.flatpak/... . The datadir is usr/share so the trigger files
# match the apex's prebuilt_usr_share entries in Android.bp. Rebuild after any
# pin change; see linux/flatpak/APEX.md.
set -euo pipefail
DEVICE=$(cd -- "$(dirname -- "$0")/../.." && pwd)
: "${MATON_AOSP:?Set MATON_AOSP to the matching AOSP checkout}"
WORK=${MATON_FLATPAK_NDK:-$MATON_AOSP/out/matonos/flatpak-ndk}
NDK=${ANDROID_NDK:-$MATON_AOSP/out/matonos/flatpak-ndk/android-ndk-r30}
PREFIX=${MATON_FLATPAK_PREFIX:-$MATON_AOSP/out/matonos/flatpak-ndk/prefix}
SOURCE=${MATON_FLATPAK_SOURCE:-$DEVICE/linux/third_party/flatpak/upstream}
SECCOMP_SOURCE=${MATON_SECCOMP_SOURCE:-$MATON_AOSP/device/maton/pc_x86_64/linux/third_party/libseccomp/upstream}
PRODUCT=${MATON_PRODUCT_OUT:-$MATON_AOSP/out/target/product/pc_x86_64}
BUILD=${MATON_FLATPAK_BUILD:-$DEVICE/linux/flatpak/build/seccomp}
JOBS=${MATON_BUILD_JOBS:-4}
[[ $JOBS =~ ^[1-4]$ ]] || { echo 'Use 1 to 4 jobs' >&2; exit 1; }
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
export PATH="$MATON_AOSP/out/matonos/flatpak-ndk/host-tools/prefix/bin:$PREFIX/bin:$PATH"
# libseccomp_matonos.so now lives inside the APEX, not in system_ext. Link the
# CLI against the Soong-built copy; both the prebuilt CLI and the APEX ship this
# exact library in lib64, resolved through the APEX linker namespace.
SECCOMP_LIB=${MATON_LIBSECCOMP_LIB:-$(find "$MATON_AOSP/out/soong/.intermediates" \
    -path '*libseccomp_matonos*' -name 'libseccomp_matonos.so' \
    ! -path '*before_final_validations*' ! -path '*unstripped*' 2>/dev/null | head -1)}
[[ -s $SECCOMP_LIB && -f $SECCOMP_SOURCE/include/seccomp.h ]] || {
  echo "libseccomp_matonos.so not found (set MATON_LIBSECCOMP_LIB); build it first" >&2; exit 1; }
[[ $(git -C "$SOURCE" rev-parse HEAD) == a02d0ba48abe9aacc377de15699e5d8c024b5669 ]]
python3 "$DEVICE/linux/third_party/verify-source.py" flatpak "$SOURCE"
mkdir -p "$BUILD"
BUILD=$(cd "$BUILD" && pwd)
mkdir -p "$BUILD/pkgconfig"
cat > "$BUILD/pkgconfig/libseccomp.pc" <<PC
Name: libseccomp
Description: Matching AOSP MatonOS libseccomp
Version: 2.5.5
Cflags: -I$SECCOMP_SOURCE/include
Libs: -L$(dirname "$SECCOMP_LIB") -lseccomp_matonos
PC
cat > "$BUILD/cross.ini" <<CROSS
[binaries]
c = '$TC/x86_64-linux-android35-clang'
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$BUILD/pkgconfig', '$PREFIX/lib64/pkgconfig', '$PREFIX/lib/pkgconfig', '$PREFIX/share/pkgconfig']
[built-in options]
c_args = ['-O2', '-fPIC', '-march=x86-64-v2']
c_link_args = ['-L$PRODUCT/system/lib64']
CROSS
setup=()
[[ ! -f $BUILD/obj/meson-private/coredata.dat ]] || setup+=(--reconfigure)
meson setup "${setup[@]}" "$BUILD/obj" "$SOURCE" --cross-file "$BUILD/cross.ini" \
    --wrap-mode=nofallback --prefix=/apex/com.matonos.flatpak \
    --libdir=lib64 --libexecdir=bin --datadir=usr/share \
    --sysconfdir=/data/matonos/linux/config --localstatedir=/data/matonos/linux \
    -Dsystem_install_dir=/data/matonos/linux/flatpak \
    -Dsystem_bubblewrap=/apex/com.matonos.flatpak/bin/matonos-bwrap \
    -Dsystem_dbus_proxy=/apex/com.matonos.flatpak/bin/xdg-dbus-proxy \
    -Dsystem_fusermount=/system/bin/fusermount3 \
    -Dseccomp=enabled -Dsystem_helper=disabled -Dsystemd=disabled \
    -Dselinux_module=disabled -Dxauth=disabled -Dgir=disabled \
    -Dgtkdoc=disabled -Ddocbook_docs=disabled -Dman=disabled \
    -Dtests=false -Ddconf=disabled -Dmalcontent=disabled \
    -Dlibzstd=disabled -Dwayland_security_context=disabled
rg -q '^#define ENABLE_SECCOMP 1$' "$BUILD/obj/config.h"
python3 "$DEVICE/linux/flatpak/tests/seccomp-syscalls-test.py" "$NDK" "$SOURCE" "$SECCOMP_SOURCE"
meson compile -C "$BUILD/obj" -j "$JOBS"
CLI=$BUILD/obj/app/flatpak
"$TC/llvm-readelf" -d "$CLI" | rg 'Shared library: \[libseccomp_matonos.so\]'
"$TC/llvm-nm" -D "$CLI" | rg ' U seccomp_export_bpf$'
"$TC/llvm-strings" "$CLI" | rg '^--seccomp$'
mkdir -p "$BUILD/stage/bin"
for pair in 'app/flatpak matonos-flatpak' 'portal/flatpak-portal flatpak-portal' \
    'revokefs/revokefs-fuse revokefs-fuse' \
    'session-helper/flatpak-session-helper flatpak-session-helper' \
    'oci-authenticator/flatpak-oci-authenticator flatpak-oci-authenticator'; do
    read -r from to <<< "$pair"
    "$TC/llvm-strip" --strip-unneeded -o "$BUILD/stage/bin/$to" "$BUILD/obj/$from"
done
# The session-helper is built for completeness, but is not shipped: host
# command execution remains unavailable. The broker emulates its safe API.
for name in matonos-flatpak flatpak-portal revokefs-fuse; do
    install -m 0755 "$BUILD/stage/bin/$name" "$DEVICE/linux/flatpak/prebuilt/system_ext/bin/$name"
done
# Retain the existing bundle's helper inventory. Newly built unshipped helpers
# remain in stage for review rather than expanding Android's runtime surface.
rg '^#define ENABLE_SECCOMP 1$' "$BUILD/obj/config.h" > "$DEVICE/linux/flatpak/seccomp-config.h"
(cd "$DEVICE/linux/flatpak/prebuilt/system_ext" && sha256sum bin/matonos-flatpak) > "$DEVICE/linux/flatpak/seccomp.sha256"

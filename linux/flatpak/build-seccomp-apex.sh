#!/usr/bin/env bash
# Build the Flatpak fork with seccomp for the com.matonos.flatpak APEX
# (r24), then stage after all checks pass.
#
# Same pipeline as build-seccomp.sh, with compiled-in paths moved from
# /system_ext to /apex/com.matonos.flatpak (bindir, libexecdir, datadir,
# FLATPAK_BWRAP and the portal's dbus-proxy path). sysconfdir and
# localstatedir stay on /data: the APEX is read-only. The meson-built
# binaries get a relative RUNPATH ($ORIGIN/../lib64) instead of the host
# build paths the /system_ext build baked in.
#
# The reviewed 1.18.4 fork working tree (uncommitted fork changes are part
# of the reviewed state) is hardlink-copied from the deps-update worktree;
# MATON_FLATPAK_APEX_SOURCE overrides it. Run BEFORE `m` so the image
# build stages the new binaries. No image is built here.
set -euo pipefail
DEVICE=$(cd -- "$(dirname -- "$0")/../.." && pwd)
: "${MATON_AOSP:?Set MATON_AOSP to the matching AOSP checkout}"
WORK=${MATON_FLATPAK_NDK:-$MATON_AOSP/out/matonos/flatpak-ndk}
NDK=${ANDROID_NDK:-$MATON_AOSP/out/matonos/flatpak-ndk/android-ndk-r30}
PREFIX=${MATON_FLATPAK_PREFIX:-$MATON_AOSP/out/matonos/flatpak-ndk/prefix}
SECCOMP_SOURCE=${MATON_SECCOMP_SOURCE:-$MATON_AOSP/device/maton/pc_x86_64/linux/third_party/libseccomp/upstream}
PRODUCT=${MATON_PRODUCT_OUT:-$MATON_AOSP/out/target/product/pc_x86_64}
BUILD=${MATON_FLATPAK_BUILD:-$DEVICE/linux/flatpak/build/apex}
JOBS=${MATON_BUILD_JOBS:-16}
[[ $JOBS =~ ^([1-9]|1[0-6])$ ]] || { echo 'Use 1 to 16 jobs' >&2; exit 1; }

# APEX mount point; every compiled-in Flatpak path hangs off it.
APEX_PREFIX=/apex/com.matonos.flatpak

# Reviewed flatpak 1.18.4 fork tree (source-pins.json: a02d0ba + uncommitted
# fork changes). Default: the deps-update worktree checkout that produced
# today's 1.18.4 binaries. Hardlink copy: read-only for this build, and the
# copy keeps working if the worktree is later pruned.
FORK_TREE=${MATON_FLATPAK_APEX_SOURCE:-$MATON_AOSP/out/worktrees/deps-update/linux/third_party/flatpak/upstream}
SOURCE=$WORK/flatpak-apex-src
[[ -d $FORK_TREE/.git ]] || { echo "Missing fork tree: $FORK_TREE" >&2; exit 1; }
[[ $(git -C "$FORK_TREE" rev-parse HEAD) == a02d0ba48abe9aacc377de15699e5d8c024b5669 ]] || {
    echo 'Fork tree is not at the pinned 1.18.4 revision' >&2; exit 1; }
rm -rf "$SOURCE"
mkdir -p "$WORK"
cp -al "$FORK_TREE" "$SOURCE" 2>/dev/null || cp -a "$FORK_TREE" "$SOURCE"

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
export PATH="$WORK/host-tools/prefix/bin:$PREFIX/bin:$PATH"
# libseccomp_matonos.so now ships inside the APEX, not system_ext. Prefer the
# Soong-built copy; fall back to a legacy system_ext staging. MATON_LIBSECCOMP_LIB
# overrides.
SECCOMP_LIB=${MATON_LIBSECCOMP_LIB:-$(find "$MATON_AOSP/out/soong/.intermediates" \
    -path '*libseccomp_matonos*' -name 'libseccomp_matonos.so' \
    ! -path '*before_final_validations*' ! -path '*unstripped*' 2>/dev/null | head -1)}
[[ -s $SECCOMP_LIB ]] || SECCOMP_LIB=$PRODUCT/system_ext/lib64/libseccomp_matonos.so
[[ -s $SECCOMP_LIB && -f $SECCOMP_SOURCE/include/seccomp.h ]] || {
    echo "libseccomp_matonos.so not found (set MATON_LIBSECCOMP_LIB); build it first" >&2; exit 1; }
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
c_link_args = ['-L$PRODUCT/system/lib64', '-Wl,-rpath,\$ORIGIN/../lib64']
CROSS
setup=()
[[ ! -f $BUILD/obj/meson-private/coredata.dat ]] || setup+=(--reconfigure)
meson setup "${setup[@]}" "$BUILD/obj" "$SOURCE" --cross-file "$BUILD/cross.ini" \
    --wrap-mode=nofallback --prefix=$APEX_PREFIX --libdir=lib64 --libexecdir=bin \
    --datadir=usr/share \
    --sysconfdir=/data/matonos/linux/config --localstatedir=/data/matonos/linux \
    -Dsystem_install_dir=/data/matonos/linux/flatpak \
    -Dsystem_bubblewrap=$APEX_PREFIX/bin/matonos-bwrap \
    -Dsystem_dbus_proxy=$APEX_PREFIX/bin/xdg-dbus-proxy \
    -Dsystem_fusermount=/system/bin/fusermount3 \
    -Dseccomp=enabled -Dsystem_helper=disabled -Dsystemd=disabled \
    -Dselinux_module=disabled -Dxauth=disabled -Dgir=disabled \
    -Dgtkdoc=disabled -Ddocbook_docs=disabled -Dman=disabled \
    -Dtests=false -Ddconf=disabled -Dmalcontent=disabled \
    -Dlibzstd=disabled -Dwayland_security_context=disabled
grep -q '^#define ENABLE_SECCOMP 1$' "$BUILD/obj/config.h"
python3 "$DEVICE/linux/flatpak/tests/seccomp-syscalls-test.py" "$NDK" "$SOURCE" "$SECCOMP_SOURCE"
meson compile -C "$BUILD/obj" -j "$JOBS"
CLI=$BUILD/obj/app/flatpak
"$TC/llvm-readelf" -d "$CLI" | grep 'Shared library: \[libseccomp_matonos.so\]'
"$TC/llvm-nm" -D "$CLI" | grep ' U seccomp_export_bpf$'
"$TC/llvm-strings" "$CLI" | grep '^--seccomp$'
# Compiled-in paths must be the APEX ones, never /system_ext. Capture once:
# `strings | grep -q` under pipefail kills strings with SIGPIPE mid-read.
STRINGS=$("$TC/llvm-strings" "$CLI")
grep -q "^$APEX_PREFIX/bin/matonos-bwrap$" <<<"$STRINGS"
grep -q "^$APEX_PREFIX/bin/xdg-dbus-proxy$" <<<"$STRINGS"
if grep -q '^/system_ext/' <<<"$STRINGS"; then
    echo 'Built CLI still contains /system_ext paths' >&2; exit 1
fi
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
grep '^#define ENABLE_SECCOMP 1$' "$BUILD/obj/config.h" > "$DEVICE/linux/flatpak/seccomp-config.h"
(cd "$DEVICE/linux/flatpak/prebuilt/system_ext" && sha256sum bin/matonos-flatpak) > "$DEVICE/linux/flatpak/seccomp.sha256"
echo "APEX-prefix Flatpak binaries staged into linux/flatpak/prebuilt/system_ext/bin"

#!/usr/bin/env bash
# Build the X11/Xwayland stack for bionic x86_64 with the pinned NDK, outside
# Soong: xorgproto, xtrans, util-macros, libpthread-stubs, xcb-proto, libXau,
# libXdmcp, libxcb, xcb-util-wm (icccm/ewmh), xcb-util-errors, libX11,
# libxkbfile, libXfont2 and Xwayland. wlroots' xwayland module needs the xcb
# libraries and the Xwayland server binary; X clients in Flatpaks need libX11.
# Sources are pinned with SHA-256 in UPSTREAMS; the EGL-only server GLX patch
# below is documented in xwayland-egl-glx.patch.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
AOSP=${MATON_AOSP_ROOT:?set MATON_AOSP_ROOT to the AOSP SDK/header checkout}
NDK=${MATON_NDK:-$HOME/Documents/android-ndk-r30}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
API=35
JOBS=${MATON_BUILD_JOBS:-4}
[[ $JOBS =~ ^[1-4]$ ]] || die 'MATON_BUILD_JOBS must be 1..4'
[[ -x $TOOLCHAIN/bin/x86_64-linux-android${API}-clang ]] || die "missing NDK at $NDK"

BUILD=${MATON_XWAYLAND_BUILD:-${MATON_COMPOSITOR_BUILD:-$AOSP/out/matonos/compositor}}
PREFIX=$BUILD/xstage/x86_64
SOURCES=${MATON_COMPOSITOR_SOURCES:-$BUILD/sources}
SRC=$SOURCES/src
DOWN=$SOURCES/downloads
COMPOSITOR_PREFIX=$BUILD/stage/x86_64
COMPOSITOR_HOST=$BUILD/host
mkdir -p "$BUILD" "$PREFIX" "$SRC"
CC=$TOOLCHAIN/bin/x86_64-linux-android${API}-clang
CXX=$TOOLCHAIN/bin/x86_64-linux-android${API}-clang++
STRIP=$TOOLCHAIN/bin/llvm-strip

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$COMPOSITOR_PREFIX/lib/pkgconfig:$COMPOSITOR_PREFIX/lib64/pkgconfig:$COMPOSITOR_PREFIX/share/pkgconfig:$COMPOSITOR_HOST/lib/pkgconfig:$COMPOSITOR_HOST/lib/x86_64-linux-gnu/pkgconfig:$COMPOSITOR_HOST/share/pkgconfig"
export PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1
# Meson sees the cross file below; autotools projects get the toolchain via
# environment variables. No sysroot flag: the NDK wrappers carry it.
export CC=$CC CXX=$CXX CPP="$CC -E" AR=$TOOLCHAIN/bin/llvm-ar \
  STRIP=$STRIP RANLIB=$TOOLCHAIN/bin/llvm-ranlib LD=$TOOLCHAIN/bin/ld.lld
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$COMPOSITOR_PREFIX/lib/pkgconfig:$COMPOSITOR_PREFIX/lib64/pkgconfig:$COMPOSITOR_PREFIX/share/pkgconfig:$COMPOSITOR_HOST/lib/pkgconfig:$COMPOSITOR_HOST/lib/x86_64-linux-gnu/pkgconfig:$COMPOSITOR_HOST/share/pkgconfig"
export PATH="$PREFIX/bin:$COMPOSITOR_HOST/bin:$PATH"

# Android provides pthreads in libc, not libpthread. Upstream projects
# unconditionally link -lpthread; an empty archive satisfies the probe, like
# the empty librt archive the compositor build provides.
mkdir -p "$PREFIX/lib"
printf 'void maton_empty_pthread_archive(void) {}\n' > "$BUILD/empty-pthread.c"
"$CC" -fPIC -c "$BUILD/empty-pthread.c" -o "$BUILD/empty-pthread.o"
"$AR" rcs "$PREFIX/lib/libpthread.a" "$BUILD/empty-pthread.o"
# <values.h> is a BSD/glibc header bionic lacks; upstream's futex helper only
# uses it for limits. A shim in our prefix satisfies it.
mkdir -p "$PREFIX/include"
cat > "$PREFIX/include/values.h" <<'EOF'
#include <limits.h>
#ifndef MAXINT
#define MAXINT INT_MAX
#endif
EOF

# Pinned archives; sizes and hashes are recorded in UPSTREAMS.
for entry in \
  'libgpg-error-1.51:be0f1b2db6b93eed55369cdf79f19f72750c8c7c39fc20b577e724545427e6b2' \
  'libgcrypt-1.11.1:24e91c9123a46c54e8371f3a3a2502f1198f2893fbfbf59af95bc1c21499b00e' \
  'xorgproto-2025.1:56898c716c0578df8a2d828c9c3e5c528277705c0484381a81960fe1a67668e8' \
  'xtrans-1.6.0:faafea166bf2451a173d9d593352940ec6404145c5d1da5c213423ce4d359e92' \
  'util-macros-1.20.2:9ac269eba24f672d7d7b3574e4be5f333d13f04a7712303b1821b2a51ac82e8e' \
  'libpthread-stubs-0.5:59da566decceba7c2a7970a4a03b48d9905f1262ff94410a649224e33d2442bc' \
  'libXau-1.0.12:74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb' \
  'libXdmcp-1.1.5:d8a5222828c3adab70adf69a5583f1d32eb5ece04304f7f8392b6a353aa2228c' \
  'xcb-proto-1.17.0:2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c' \
  'libxcb-1.17.0:599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55' \
  'xcb-util-wm-0.4.2:62c34e21d06264687faea7edbf63632c9f04d55e72114aa4a57bb95e4f888a0b' \
  'xcb-util-errors-1.0.1:5628c87b984259ad927bacd8a42958319c36bdf4b065887803c9d820fb80f357' \
  'libX11-1.8.13:69606f485c2c07c14ef64f75b7bb326d48587af33795d9ab3e607c0b5f94f11c' \
  'libxkbfile-1.2.0:7f71884e5faf56fb0e823f3848599cf9b5a9afce51c90982baeb64f635233ebf' \
  'libXfont2-2.0.9:f042a370666815e7b941e9b7019024755bd1c6c2954afbfa515af378251799e2' \
  'libfontenc-1.1.0:8597e761b7e3624fbf8de4b4f19b9927822f24eeaf15524dc5dbbaf6ae7d8dd1' \
  'libxcvt-0.1.3:a929998a8767de7dfa36d6da4751cdbeef34ed630714f2f4a767b351f2442e01' \
  'libxshmfence-1.3.3:d4a4df096aba96fea02c029ee3a44e11a47eb7f7213c1a729be83e85ec3fde10' \
  'freetype-2.14.1:32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc' \
  'xkbcomp-1.5.0:2ac31f26600776db6d9cd79b3fcd272263faebac7eb85fb2f33c7141b8486060' \
  'libepoxy-1.5.10:a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15' \
  'xkeyboard-config-2.48:b77041324f0109f77161ee43743fe04baa485866af8460d31e476ad3f7648fd5' \
  'xwayland-24.1.13:173aea3d6f79609164c04528e1c8e4c9b60fcd59391c3c9dad4667297d727fb6'; do
  name=${entry%%:*}; hash=${entry#*:}
  if [[ ${MATON_X11_CLIENTS_ONLY:-0} == 1 ]]; then
    case "$name" in xorgproto-2025.1|xtrans-1.6.0|util-macros-1.20.2|libpthread-stubs-0.5|xcb-proto-1.17.0|libXau-1.0.12|libXdmcp-1.1.5|libxcb-1.17.0|xcb-util-wm-0.4.2|xcb-util-errors-1.0.1) ;; *) continue;; esac
  fi
  archive=$DOWN/$name.tar.xz
  [[ -s $archive ]] || archive=$DOWN/$name.tar.gz
  [[ -s $archive ]] || archive=$DOWN/$name.tar.bz2
  [[ -s $archive ]] || die "missing pinned source archive $archive"
  echo "$hash  $archive" | sha256sum -c - >/dev/null || die "checksum mismatch: $name"
done

# Autotools cross build helper. Bionic needs _GNU_SOURCE-free POSIX builds;
# the NDK wrapper defines its own defaults.
configure_build() { # name configure-args...
  local name=$1; shift
  local src=$SRC/$name
  [[ -d $src ]] || { tar -xf "$DOWN/$name.tar.xz" -C "$SRC" 2>/dev/null ||
    tar -xzf "$DOWN/$name.tar.gz" -C "$SRC"; }
  if [[ ! -f $src/config.status ]]; then
    (cd "$src" && ./configure --host=x86_64-linux-android --prefix="$PREFIX" "$@") || die "configure failed: $name"
  fi
  make -C "$src" -j"$JOBS" || die "make failed: $name"
  make -C "$src" install || die "install failed: $name"
  echo "== $name installed"
}

# Meson cross build helper for projects with meson support.
configure_meson() { # name src args...
  local name=$1 src=$2; shift 2
  local dir=$BUILD/xwayland-stack/$name
  [[ -d $src ]] || tar -xf "$DOWN/$(basename "$src").tar.xz" -C "$SRC"
  local reconfigure=(); [[ -f $dir/build.ninja ]] && reconfigure=(--reconfigure)
  meson setup "${reconfigure[@]}" "$dir" "$src" --cross-file "$BUILD/android-x86_64-x.ini" \
    --prefix="$PREFIX" --libdir=lib --buildtype=release "$@"
  meson compile -C "$dir" -j"$JOBS"
  meson install -C "$dir" >/dev/null
  echo "== $name installed"
}

cat > "$BUILD/android-x86_64-x.ini" <<EOF
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
c_link_args = ['-L$PREFIX/lib']
EOF

# xorgproto: all X protocol headers + the *proto pc files (incl. xwaylandproto).
configure_build xorgproto-2025.1 --without-xmlto --without-fop
# xtrans: transport headers only.
configure_build xtrans-1.6.0
# util-macros: X.Org autotools macros (installed into the shared prefix).
configure_build util-macros-1.20.2
# libpthread-stubs: pc file only; bionic has pthreads.
configure_build libpthread-stubs-0.5
# xcb-proto: protocol XML + the xcbgen python module used by libxcb's build.
PYTHON=python3 configure_build xcb-proto-1.17.0
# libXau/libXdmcp: X connection auth helpers libxcb requires.
configure_build libXau-1.0.12
configure_build libXdmcp-1.1.5
# libxcb: the X protocol C bindings incl. composite/render/res/xfixes/xkb
# extensions wlroots' xwayland module uses. Threads disabled: bionic provides
# pthreads in libc and libxcb's pthread-stubs probe would fail otherwise.
PYTHON=python3 configure_build libxcb-1.17.0 --without-xcb-xprint \
  --disable-sendfds --disable-xcms
# xcb-util-wm (icccm + ewmh) and xcb-util-errors: wlroots xwm requirements.
configure_build xcb-util-wm-0.4.2
configure_build xcb-util-errors-1.0.1
if [[ ${MATON_X11_CLIENTS_ONLY:-0} == 1 ]]; then
cat > "$PREFIX/lib/pkgconfig/xwayland.pc" <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Xwayland
Description: X Server for Wayland
Version: 24.1.13
URL: https://gitlab.freedesktop.org/xorg/xserver/
# The device runtime path: wlroots bakes this into XWAYLAND_PATH and checks
# access(X_OK) on it at server creation. The build prefix would fail there.
xwayland=libmatonos-Xwayland.so
have_glamor=true
have_glamor_api=true
have_eglstream=false
have_initfd=true
have_listenfd=true
have_verbose=true
have_terminate_delay=true
have_no_touch_pointer_emulation=true
have_force_xrandr_emulation=true
have_geometry=true
have_fullscreen=true
have_host_grab=true
have_decorate=false
have_enable_ei_portal=false
have_byteswappedclients=true
have_hidpi=true
EOF
exit 0
fi
# libX11: the client-side Xlib for Flatpak apps (links our libxcb).
configure_build libX11-1.8.13 --disable-udb
# libxkbfile is meson-only (no configure script); libXfont2 still ships
# autotools. Both are Xwayland's XKB and font backend requirements.
configure_meson libxkbfile "$SRC/libxkbfile-1.2.0"
# freetype: required by libXfont2. Core glyph rendering only; PNG, Bzip2,
# Brotli and HarfBuzz stay disabled.
configure_meson freetype "$SRC/freetype-2.14.1" \
  -Dpng=disabled -Dbzip2=disabled -Dbrotli=disabled -Dharfbuzz=disabled \
  -Dzlib=disabled -Dtests=disabled
# libfontenc: compressed font support libXfont2 requires.
configure_build libfontenc-1.1.0
configure_build libXfont2-2.0.9
# libxcvt: CVT modeline math split out of the X server, required by Xwayland.
configure_meson libxcvt "$SRC/libxcvt-0.1.3"
# libxshmfence: MIT-SHM sync fences; miext/sync includes its header.
configure_build libxshmfence-1.3.3
# Build SHA1 dependencies here, independently of the musl Flatpak stack.
# Generate the lock ABI from an NDK-compiled PTHREAD_MUTEX_INITIALIZER object;
# upstream configure only invokes this generator automatically for GNU/musl.
for name in libgpg-error-1.51 libgcrypt-1.11.1; do
  [[ -d $SRC/$name ]] || tar -xf "$DOWN/$name.tar.bz2" -C "$SRC"
done
GPG_SRC=$SRC/libgpg-error-1.51
(cd "$GPG_SRC" && LOCK_ABI_VERSION=1 host=x86_64-unknown-linux-android \
  host_alias=x86_64-linux-android CC="$CC" OBJDUMP="$TOOLCHAIN/bin/llvm-objdump" \
  ac_ext=c ac_objext=o AWK=awk sh src/gen-lock-obj.sh \
  > src/syscfg/lock-obj-pub.x86_64-unknown-linux-android.h)
# Fail early if the generator could not read the target object's size.
grep -Eq '_priv\[[1-9][0-9]*\]' "$GPG_SRC/src/syscfg/lock-obj-pub.x86_64-unknown-linux-android.h" ||
  die 'cannot determine bionic pthread mutex size'
# -avoid-version keeps both the filename and DT_SONAME unversioned for APK JNI.
# Limit recursive builds to the library directories (no tools/docs/tests).
crypto_build() {
  local name=$1 dirs=$2; shift 2
  local src=$SRC/$name
  if [[ ! -f $src/config.status ]]; then
    (cd "$src" && ./configure --host=x86_64-linux-android --prefix="$PREFIX" \
      --libdir="$PREFIX/lib" --enable-shared --disable-static --disable-doc "$@")
  fi
  make -C "$src" -j"$JOBS" SUBDIRS="$dirs" extra_ltoptions=-avoid-version
  make -C "$src" install SUBDIRS="$dirs" extra_ltoptions=-avoid-version
}
crypto_build libgpg-error-1.51 'src' --disable-tests --disable-nls
crypto_build libgcrypt-1.11.1 'compat mpi cipher random src' \
  --with-libgpg-error-prefix="$PREFIX" --disable-asm

# GPU rendering (glamor + DRI3). Mesa lives in /vendor and its Android build
# has no EGL GBM platform, so Xwayland gets three small forwarding libraries
# (xwayland-egl/): libEGL, libGLESv2 and libgbm load Mesa through the
# same-process-HAL namespace, and libEGL maps the GBM platform to
# EGL_EXT_platform_device. They sit in lib/xwayland-egl (on device
# private runtime library links), which only Xwayland's LD_LIBRARY_PATH names.
MESA_SRC=${MESA_DIR:-$HOME/Documents/mesa-26.2.3}
[[ -f $MESA_SRC/src/gbm/main/gbm.h ]] || die "Mesa source (gbm.h) missing at $MESA_SRC"
SHIM_SRC=$ROOT/xwayland-egl
SHIM=$PREFIX/lib/xwayland-egl
SYSINC=$TOOLCHAIN/sysroot/usr/include
mkdir -p "$SHIM" "$BUILD/xwayland-egl" "$PREFIX/include"
python3 "$SHIM_SRC/gen_forward.py" "$BUILD/xwayland-egl/egl_forward.c" libEGL_mesa.so 'egl[A-Z][A-Za-z0-9]*' \
  --skip eglGetPlatformDisplay,eglQueryString,eglGetProcAddress "$SYSINC/EGL/egl.h"
python3 "$SHIM_SRC/gen_forward.py" "$BUILD/xwayland-egl/gles_forward.c" libGLESv2_mesa.so 'gl[A-Z][A-Za-z0-9]*' \
  "$SYSINC/GLES2/gl2.h" "$SYSINC/GLES3/gl3.h" "$SYSINC/GLES3/gl31.h" "$SYSINC/GLES3/gl32.h"
python3 "$SHIM_SRC/gen_forward.py" "$BUILD/xwayland-egl/gbm_forward.c" libgbm_mesa.so 'gbm_[a-z0-9_]+' \
  "$MESA_SRC/src/gbm/main/gbm.h"
shim_lib() { # soname sources...
  local soname=$1; shift
  "$CC" -O2 -Wall -fPIC -shared -I"$SHIM_SRC" -o "$SHIM/$soname" -Wl,-soname,"$soname" "$@" \
    "$SHIM_SRC/sphal.c" -ldl -llog || die "build failed: $soname"
}
shim_lib libEGL.so "$BUILD/xwayland-egl/egl_forward.c" "$SHIM_SRC/egl_overrides.c"
shim_lib libGLESv2.so "$BUILD/xwayland-egl/gles_forward.c"
shim_lib libgbm.so "$BUILD/xwayland-egl/gbm_forward.c"
install -m 0644 "$MESA_SRC/src/gbm/main/gbm.h" "$PREFIX/include/gbm.h"
cat > "$PREFIX/lib/pkgconfig/gbm.pc" <<EOF
prefix=$PREFIX
Name: gbm
Description: Mesa gbm (MatonOS forwarder to the vendor Mesa)
Version: 26.2.3
Libs: -L$SHIM -lgbm
Cflags: -I$PREFIX/include
EOF
# libepoxy: GL dispatch for glamor; on Android it opens libEGL.so/libGLESv2.so.
[[ -d $SRC/libepoxy-1.5.10 ]] || tar -xzf "$DOWN/libepoxy-1.5.10.tar.gz" -C "$SRC"
configure_meson libepoxy "$SRC/libepoxy-1.5.10" -Degl=yes -Dglx=no -Dx11=false -Dtests=false

# Server GLX uses epoxy dispatch and glamor's existing EGL display, not
# libGL or a second Mesa/DRI module installation. Meson's GLX dependency('gl')
# only needs the Khronos GL headers here; provide a headers-only pc file.
# Use the same Mesa source as the vendor forwarders (NDK supplies KHR headers).
mkdir -p "$PREFIX/include/GL"
for header in gl.h glext.h glcorearb.h; do
  install -m 0644 "$MESA_SRC/include/GL/$header" "$PREFIX/include/GL/$header"
done
cat > "$PREFIX/lib/pkgconfig/gl.pc" <<EOF
prefix=$PREFIX
includedir=\${prefix}/include
Name: gl
Description: Khronos GL headers only (Xwayland server uses epoxy/EGL)
Version: 1.2
Libs:
Cflags: -I\${includedir}
EOF
# Xwayland: the standalone X server for Wayland (meson). glamor and DRI3 run
# on the GPU through the forwarders above. Enabling GLX also builds
# glamor/glamor_glx_provider.c; xwl_glamor_init pushes this EGL-backed provider.
# The patch uses epoxy for server GL dispatch and removes the DRI fallback.
# Keep indirect GLX disabled (the default):
# the EGL provider supports direct clients and needs no *_dri.so modules.
# SHA1 comes from the bionic libgcrypt built into this Xwayland prefix.
# MIT-SHM on, fd path only: the kernel has no SysV IPC and each app's SysV
# segments live in the v3 proxy's virtual view, which Xwayland cannot see.
# shm.c's classic ShmAttach calls shmat/shmctl; the app seccomp filter traps
# those with SIGSYS (not ENOSYS), which would abort the server, so
# xwayland-shm-nosysv.patch makes ProcShmAttach return a clean BadAccess
# without calling them. ShmAttachFd and ShmCreateSegment carry a memfd over
# SCM_RIGHTS (SHM_FD_PASSING from XTRANS_SEND_FDS) and mmap it, so clients
# that speak MIT-SHM 1.2 still get shared-memory image transfers.
[[ -d $SRC/xwayland-24.1.13 ]] || tar -xf "$DOWN/xwayland-24.1.13.tar.xz" -C "$SRC"
GLX_PATCH=$ROOT/xwayland-egl-glx.patch
if patch --dry-run --batch --forward -p1 -d "$SRC/xwayland-24.1.13" < "$GLX_PATCH" >/dev/null 2>&1; then
  patch --batch --forward -p1 -d "$SRC/xwayland-24.1.13" < "$GLX_PATCH"
elif ! patch --dry-run --batch --reverse -p1 -d "$SRC/xwayland-24.1.13" < "$GLX_PATCH" >/dev/null 2>&1; then
  die 'Xwayland EGL GLX patch neither applies nor is already applied'
fi
# Android apps' seccomp filter kills setgid/setuid: xkbcomp's Popen child.
IDS_PATCH=$ROOT/xwayland-popen-ids.patch
if patch --dry-run --batch --forward -p1 -d "$SRC/xwayland-24.1.13" < "$IDS_PATCH" >/dev/null 2>&1; then
  patch --batch --forward -p1 -d "$SRC/xwayland-24.1.13" < "$IDS_PATCH"
elif ! patch --dry-run --batch --reverse -p1 -d "$SRC/xwayland-24.1.13" < "$IDS_PATCH" >/dev/null 2>&1; then
  die 'Xwayland Popen ids patch neither applies nor is already applied'
fi
# No SysV IPC on Android: classic ShmAttach's shmat/shmctl are trapped by
# seccomp (SIGSYS), so return a clean BadAccess instead of aborting. The fd
# path (ShmAttachFd/ShmCreateSegment) still provides shared-memory transfers.
SHM_PATCH=$ROOT/xwayland-shm-nosysv.patch
if patch --dry-run --batch --forward -p1 -d "$SRC/xwayland-24.1.13" < "$SHM_PATCH" >/dev/null 2>&1; then
  patch --batch --forward -p1 -d "$SRC/xwayland-24.1.13" < "$SHM_PATCH"
elif ! patch --dry-run --batch --reverse -p1 -d "$SRC/xwayland-24.1.13" < "$SHM_PATCH" >/dev/null 2>&1; then
  die 'Xwayland shm-nosysv patch neither applies nor is already applied'
fi
configure_meson xwayland "$SRC/xwayland-24.1.13" \
  -Dglx=true -Dglamor=true -Dxv=false -Ddri3=true -Dmitshm=true \
  -Dsecure-rpc=false \
  -Dxwayland_ei=false -Dxdmcp=false -Dsystemd_notify=false \
  -Dlibdecor=false -Dxvfb=false -Dsha1=libgcrypt \
  -Dxwayland-path="$PREFIX/bin" \
  -Dxkb_dir=/system_ext/share/X11/xkb -Dxkb_bin_dir=. \
  -Dxkb_output_dir=/system_ext/share/X11/xkb/compiled

# Xwayland cannot start without a keymap: it runs xkbcomp on the XKB data at
# server start. The read-only xkb_output_dir makes it fall back to
# $XDG_RUNTIME_DIR, which the compositor points at its private runtime dir.
configure_build xkbcomp-1.5.0
# xkeyboard-config is data only; built for the host into its own prefix and
# staged verbatim (arch independent).
XKB_DATA=$BUILD/xkb-data
[[ -d $SRC/xkeyboard-config-2.48 ]] || tar -xf "$DOWN/xkeyboard-config-2.48.tar.xz" -C "$SRC"
rm -rf "$BUILD/build-xkeyboard-config"
env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR meson setup "$BUILD/build-xkeyboard-config" \
  "$SRC/xkeyboard-config-2.48" --prefix=/system_ext -Dxorg-rules-symlinks=true -Dnls=false >/dev/null ||
  die "configure failed: xkeyboard-config"
rm -rf "$XKB_DATA"
DESTDIR=$XKB_DATA meson install -C "$BUILD/build-xkeyboard-config" >/dev/null || die "install failed: xkeyboard-config"
[[ -f $XKB_DATA/system_ext/share/xkeyboard-config-2/rules/evdev ]] || die "xkeyboard-config: X11 rules missing"

# wlroots reads the xwayland.pc dependency and its have_* feature variables.
# Upstream's generated pc file is replicated here verbatim.
cat > "$PREFIX/lib/pkgconfig/xwayland.pc" <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Xwayland
Description: X Server for Wayland
Version: 24.1.13
URL: https://gitlab.freedesktop.org/xorg/xserver/
# The device runtime path: wlroots bakes this into XWAYLAND_PATH and checks
# access(X_OK) on it at server creation. The build prefix would fail there.
xwayland=libmatonos-Xwayland.so
have_glamor=true
have_glamor_api=true
have_eglstream=false
have_initfd=true
have_listenfd=true
have_verbose=true
have_terminate_delay=true
have_no_touch_pointer_emulation=true
have_force_xrandr_emulation=true
have_geometry=true
have_fullscreen=true
have_host_grab=true
have_decorate=false
have_enable_ei_portal=false
have_byteswappedclients=true
have_hidpi=true
EOF

echo "X11/Xwayland stack installed under $PREFIX"
ls -la "$PREFIX/bin/Xwayland" 2>/dev/null || die "Xwayland binary missing after build"

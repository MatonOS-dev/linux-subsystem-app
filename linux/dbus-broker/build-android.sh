#!/usr/bin/env bash
set -Eeuo pipefail

DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
AOSP=${MATON_AOSP_ROOT:-$(cd "$DEVICE_DIR/../../.." && pwd)}
FLATPAK_NDK=${MATON_FLATPAK_NDK:-$AOSP/out/matonos/flatpak-ndk}
NDK=${MATON_ANDROID_NDK:-$FLATPAK_NDK/android-ndk-r30}
GLIB_SRC=${MATON_GLIB_SOURCE:-$FLATPAK_NDK/glib/source}
WORK=${MATON_DBUS_ANDROID_WORK:-$AOSP/out/matonos/flatpak-ndk/dbus-static}
OUT=${MATON_DBUS_ANDROID_OUT:-$AOSP/out/pc-logs/dbus-broker/android}
JOBS=${MATON_BUILD_JOBS:-4}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
MESON=${MATON_MESON:-/usr/bin/meson}
NINJA=${MATON_NINJA:-$(command -v ninja)}
CMAKE=${MATON_CMAKE:-$AOSP/prebuilts/cmake/linux-x86/bin/cmake}

for path in "$TOOLCHAIN/x86_64-linux-android35-clang" "$GLIB_SRC/meson.build" \
            "$AOSP/external/pcre/CMakeLists.txt" "$AOSP/external/libffi/gen_ffi_header.sh"; do
  [[ -e $path ]] || { echo "missing NDK/Flatpak build input: $path" >&2; exit 1; }
done
[[ $JOBS =~ ^[1-4]$ ]] || { echo "MATON_BUILD_JOBS must be between 1 and 4" >&2; exit 1; }

# Read the generated configuration of the same Flatpak build staged in the
# image. Do not use a host distro Flatpak or an independently numbered protocol.
FLATPAK_CONFIG=${MATON_FLATPAK_BUILD_CONFIG:-$FLATPAK_NDK/flatpak/build/config.h}
FLATPAK_VERSION=$(sed -n 's/^#define PACKAGE_VERSION "\([0-9][0-9.]*\)"$/\1/p' "$FLATPAK_CONFIG")
[[ $FLATPAK_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "Cannot determine image Flatpak version from $FLATPAK_CONFIG" >&2; exit 1;
}
mkdir -p "$OUT"
printf '#define MATON_BROKER_BUILT_FLATPAK_VERSION "%s"\n' "$FLATPAK_VERSION" > "$OUT/flatpak-build-version.h"
printf '%s\n' "$FLATPAK_VERSION" > "$OUT/flatpak-build-version.txt"

GLIB_BUILD=$WORK/glib-build
GLIB_PREFIX=$WORK/prefix
if [[ ! -f $GLIB_BUILD/build.ninja ]]; then
  "$MESON" setup "$GLIB_BUILD" "$GLIB_SRC" \
    --cross-file "$FLATPAK_NDK/android-x86_64-api35.ini" \
    --prefix="$GLIB_PREFIX" --libdir=lib64 --buildtype=release \
    -Ddefault_library=static -Dtests=false -Dintrospection=disabled \
    -Dman-pages=disabled -Dnls=disabled -Dselinux=disabled \
    -Dlibmount=disabled -Dlibelf=disabled -Ddtrace=false \
    -Dsystemtap=false -Dsysprof=disabled -Dglib_debug=disabled \
    -Dforce_posix_threads=true
fi
"$NINJA" -C "$GLIB_BUILD" -j"$JOBS"
"$NINJA" -C "$GLIB_BUILD" install

PCRE_BUILD=$WORK/pcre-build
"$CMAKE" -S "$AOSP/external/pcre" -B "$PCRE_BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-35 \
  -DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_PCRE2_8=ON \
  -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF \
  -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF \
  -DPCRE2_BUILD_PCRE2GREP=OFF
"$NINJA" -C "$PCRE_BUILD" -j"$JOBS"

# Build only the x86_64 libffi archive needed by static GObject. Use AOSP's
# source/config headers but put all generated files and objects under out/.
FFI_SRC=$AOSP/external/libffi
FFI_BUILD=$WORK/libffi-build
mkdir -p "$FFI_BUILD/include" "$FFI_BUILD/obj"
bash "$FFI_SRC/gen_ffi_header.sh" < "$FFI_SRC/include/ffi.h.in" > "$FFI_BUILD/include/ffi_gen.h"
for file in closures debug java_raw_api prep_cif raw_api types; do
  "$TOOLCHAIN/x86_64-linux-android35-clang" -O2 -fPIC \
    -I"$FFI_SRC/include" -I"$FFI_SRC/linux-x86_64" -I"$FFI_SRC/src/x86" \
    -I"$FFI_BUILD/include" -c "$FFI_SRC/src/$file.c" -o "$FFI_BUILD/obj/$file.o"
done
for file in ffi64.c ffiw64.c; do
  "$TOOLCHAIN/x86_64-linux-android35-clang" -O2 -fPIC \
    -I"$FFI_SRC/include" -I"$FFI_SRC/linux-x86_64" -I"$FFI_SRC/src/x86" \
    -I"$FFI_BUILD/include" -c "$FFI_SRC/src/x86/$file" \
    -o "$FFI_BUILD/obj/${file%.c}.o"
done
for file in unix64.S win64.S; do
  "$TOOLCHAIN/x86_64-linux-android35-clang" -DHAVE_AS_X86_PCREL \
    -DHAVE_AS_ASCII_PSEUDO_OP -I"$FFI_SRC/include" \
    -I"$FFI_SRC/linux-x86_64" -I"$FFI_SRC/src/x86" \
    -I"$FFI_BUILD/include" -c "$FFI_SRC/src/x86/$file" \
    -o "$FFI_BUILD/obj/${file%.S}.o"
done
"$TOOLCHAIN/llvm-ar" rcs "$FFI_BUILD/libffi.a" "$FFI_BUILD"/obj/*.o

mkdir -p "$OUT"
INCLUDES=(-I"$GLIB_PREFIX/include/glib-2.0" \
  -I"$GLIB_PREFIX/lib64/glib-2.0/include" -I"$GLIB_PREFIX/include" \
  -I"$GLIB_PREFIX/include/gio-unix-2.0")
CFLAGS=(-O2 -g -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror -include "$OUT/flatpak-build-version.h")
for source in main broker example portals test-client session-test-client; do
  "$TOOLCHAIN/x86_64-linux-android35-clang" "${CFLAGS[@]}" "${INCLUDES[@]}" \
    -c "$DEVICE_DIR/linux/dbus-broker/$source.c" -o "$OUT/$source.o"
done

STATIC_LIBS=("$GLIB_PREFIX/lib64/libgio-2.0.a" \
  "$GLIB_PREFIX/lib64/libgobject-2.0.a" "$GLIB_PREFIX/lib64/libgmodule-2.0.a" \
  "$GLIB_PREFIX/lib64/libglib-2.0.a" "$PCRE_BUILD/libpcre2-8.a" \
  "$FFI_BUILD/libffi.a" "$GLIB_PREFIX/lib64/libintl.a" -lz -ldl -lm)
"$TOOLCHAIN/x86_64-linux-android35-clang" -o "$OUT/matonos-dbus-broker" \
  "$OUT/main.o" "$OUT/broker.o" "$OUT/example.o" "$OUT/portals.o" "${STATIC_LIBS[@]}"
"$TOOLCHAIN/x86_64-linux-android35-clang" -o "$OUT/matonos-dbus-test-client" \
  "$OUT/test-client.o" "${STATIC_LIBS[@]}"
"$TOOLCHAIN/x86_64-linux-android35-clang" -o "$OUT/matonos-dbus-session-test" \
  "$OUT/session-test-client.o" "${STATIC_LIBS[@]}"
"$TOOLCHAIN/llvm-strip" "$OUT/matonos-dbus-broker" "$OUT/matonos-dbus-test-client" "$OUT/matonos-dbus-session-test"
echo "Built static-GIO bionic binaries in $OUT"

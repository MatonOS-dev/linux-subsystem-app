#!/usr/bin/env bash
set -Eeuo pipefail

DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
AOSP=${MATON_AOSP_ROOT:-$(cd "$DEVICE_DIR/../../.." && pwd)}
FLATPAK_NDK=${MATON_FLATPAK_NDK:-$AOSP/out/matonos/flatpak-ndk}
NDK=${MATON_ANDROID_NDK:-${MATON_NDK:?set MATON_NDK}}
PREFIX=${MATON_FLATPAK_PREFIX:?set MATON_FLATPAK_PREFIX to the pinned Flatpak dependency prefix}
OUT=${MATON_DBUS_ANDROID_OUT:?set MATON_DBUS_ANDROID_OUT}
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
GLIB_PREFIX=$PREFIX
PCRE_BUILD=$PREFIX/lib
FFI_BUILD=$PREFIX/lib
mkdir -p "$OUT"
INCLUDES=(-I"$GLIB_PREFIX/include/glib-2.0" \
  -I"$GLIB_PREFIX/lib/glib-2.0/include" -I"$GLIB_PREFIX/include" \
  -I"$GLIB_PREFIX/include/gio-unix-2.0")
CFLAGS=(-O2 -g -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror)
for source in broker portals maton_broker_jni; do
  "$TOOLCHAIN/x86_64-linux-android35-clang" "${CFLAGS[@]}" "${INCLUDES[@]}" \
    -c "$DEVICE_DIR/linux/dbus-broker/$source.c" -o "$OUT/$source.o"
done
STATIC_LIBS=("$GLIB_PREFIX/lib/libgio-2.0.a" \
  "$GLIB_PREFIX/lib/libgobject-2.0.a" "$GLIB_PREFIX/lib/libgmodule-2.0.a" \
  "$GLIB_PREFIX/lib/libglib-2.0.a" "$PCRE_BUILD/libpcre2-8.a" \
  "$FFI_BUILD/libffi.a" -lz -ldl -lm)
# Per-app bus: a JNI library loaded into the app's own process, not an executable.
"$TOOLCHAIN/x86_64-linux-android35-clang" -shared -o "$OUT/libmatonos-dbus-broker.so" \
  "$OUT/maton_broker_jni.o" "$OUT/broker.o" "$OUT/portals.o" \
  "${STATIC_LIBS[@]}"
"$TOOLCHAIN/llvm-strip" "$OUT/libmatonos-dbus-broker.so"
echo "Built per-app JNI broker in $OUT"

#!/usr/bin/env bash
# Build unmodified upstream code with the existing Flatpak NDK dependencies.
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
WORK=${MATON_FLATPAK_NDK:-$ROOT/out/matonos/flatpak-ndk}
REV=1c1989e56f94b9eb3b7567f8a6e8a0aa16cba496
SOURCE=$WORK/xdg-dbus-proxy-src
BUILD=$WORK/xdg-dbus-proxy-build
if [[ ! -d $SOURCE/.git ]]; then
  git clone --depth 1 --branch 0.1.6 https://github.com/flatpak/xdg-dbus-proxy.git "$SOURCE"
fi
[[ $(git -C "$SOURCE" rev-parse HEAD) == "$REV" ]] || { echo 'Unexpected xdg-dbus-proxy revision' >&2; exit 1; }
[[ -z $(git -C "$SOURCE" status --porcelain) ]] || { echo 'Upstream source is modified' >&2; exit 1; }
meson setup --reconfigure "$BUILD" "$SOURCE" \
  --cross-file "$WORK/android-x86_64-api35.ini" --prefix /system_ext \
  -Dtests=false -Dman=disabled
meson compile -C "$BUILD" -j "${MATON_BUILD_JOBS:-16}"
install -m 0755 "$BUILD/xdg-dbus-proxy" "$HERE/prebuilt/system_ext/bin/xdg-dbus-proxy"

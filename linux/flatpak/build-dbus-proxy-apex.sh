#!/usr/bin/env bash
# Build unmodified upstream xdg-dbus-proxy for the com.matonos.flatpak APEX.
# Same pipeline as build-dbus-proxy.sh with the APEX prefix and a relative
# RUNPATH, matching build-seccomp-apex.sh. No baked paths exist in this
# tool; the rebuild keeps the APEX binaries consistent.
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=${MATON_AOSP:-$(cd "$HERE/../../../../.." && pwd)}
WORK=${MATON_FLATPAK_NDK:-$ROOT/out/matonos/flatpak-ndk}
REV=72b40c8f6d9d585cfbdb249690724f740d207087
# Release archive: xdg-dbus-proxy-0.1.9.tar.xz
# SHA-256: 5450dda586ec3bb3ca709d311e845487883faa3b09cf562608d7e84f4311dced
SOURCE=${MATON_DBUS_PROXY_SOURCE:-$WORK/xdg-dbus-proxy-src}
BUILD=$WORK/xdg-dbus-proxy-build-apex
if [[ ! -d $SOURCE/.git ]]; then
  git clone --depth 1 --branch 0.1.9 https://github.com/flatpak/xdg-dbus-proxy.git "$SOURCE"
fi
[[ $(git -C "$SOURCE" rev-parse HEAD) == "$REV" ]] || { echo 'Unexpected xdg-dbus-proxy revision' >&2; exit 1; }
[[ -z $(git -C "$SOURCE" status --porcelain) ]] || { echo 'Upstream source is modified' >&2; exit 1; }
python3 "$HERE/../third_party/verify-source.py" xdg-dbus-proxy "$SOURCE"
JOBS=${MATON_BUILD_JOBS:-16}
[[ $JOBS =~ ^([1-9]|1[0-6])$ ]] || { echo 'Use 1 to 16 jobs' >&2; exit 1; }
setup=()
[[ ! -f $BUILD/meson-private/coredata.dat ]] || setup+=(--reconfigure)
meson setup "${setup[@]}" "$BUILD" "$SOURCE" \
  --cross-file "$WORK/android-x86_64-api35.ini" --prefix /apex/com.matonos.flatpak \
  -Dtests=false -Dman=disabled -Dc_args=-march=x86-64-v2 \
  -Dc_link_args=-Wl,-rpath,'$ORIGIN/../lib64'
meson compile -C "$BUILD" -j "$JOBS"
install -m 0755 "$BUILD/xdg-dbus-proxy" "$HERE/prebuilt/system_ext/bin/xdg-dbus-proxy"

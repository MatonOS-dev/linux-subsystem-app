#!/usr/bin/env bash
set -Eeuo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
SCRATCH=${MUSL_SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
WORK=${FLATPAK_MUSL_WORK:-$SCRATCH/flatpak-musl}
HOST_CACHE=${CACHE_DIR:-$WORK/cache}
HOST_BUILD=${BUILD_DIR:-$WORK/build}
HOST_OUT=${OUT_DIR:-$WORK/out}
for path in "$HOST_CACHE" "$HOST_BUILD" "$HOST_OUT"; do
  case "$(realpath -m "$path")" in
    "$(realpath -m "$SCRATCH")"/*) ;;
    *) echo "Flatpak musl work paths must be under MUSL_SCRATCH: $path" >&2; exit 2;;
  esac
done
mkdir -p "$HOST_CACHE" "$HOST_BUILD" "$HOST_OUT"
inner_path() { printf '/scratch/%s' "${1#"$(realpath -m "$SCRATCH")"/}"; }
SCRATCH="$SCRATCH" GPGME_LITE_SRC="$HOST_CACHE/src/DullPGP" \
  CACHE_DIR="$(inner_path "$HOST_CACHE")" \
  BUILD_DIR="$(inner_path "$HOST_BUILD")" OUT_DIR="$(inner_path "$HOST_OUT")" \
  JOBS=${JOBS:-4} START_AT=${START_AT:-all} \
  "$HERE/alpine-enter.sh" /work/build.sh
install -m755 "$HOST_OUT/matonos-flatpak" "$HERE/../prebuilt/static/x86_64/matonos-flatpak"
install -m644 "$HOST_OUT/SOURCE" "$HERE/../prebuilt/static/SOURCE"
bash "$HERE/../../../tools/build-flatpak-helpers.sh"
{
  sed -E '/^[0-9a-f]{64}  matonos-flatpak$/d' "$HOST_OUT/SOURCE"
  echo
  echo 'static musl APK helper artifacts'
  (cd "$HERE/../prebuilt/static/x86_64" && sha256sum matonos-flatpak matonos-app-flatpak matonos-app-bwrap)
  awk '$2 != "matonos-app-bwrap"' "$HERE/../prebuilt/static/x86_64/HELPERS.sha256"
} > "$HOST_OUT/SOURCE.complete"
install -m644 "$HOST_OUT/SOURCE.complete" "$HERE/../prebuilt/static/SOURCE"

#!/usr/bin/env bash
set -Eeuo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
SCRATCH=${MUSL_SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
WORK=${FLATPAK_MUSL_WORK:-$SCRATCH/flatpak-musl}
HOST_CACHE=${CACHE_DIR:-$WORK/cache}
HOST_BUILD=${BUILD_DIR:-$WORK/build}
HOST_OUT=${OUT_DIR:-$WORK/out}
case "$(realpath -m "$HOST_CACHE")" in
  "$(realpath -m "$SCRATCH")"/*) ;;
  *) echo "Flatpak musl cache must be under MUSL_SCRATCH: $HOST_CACHE" >&2; exit 2;;
esac
for path in "$HOST_BUILD" "$HOST_OUT"; do
  case "$(realpath -m "$path")" in
    "$(realpath -m "$WORK")"/*) ;;
    *) echo "Flatpak musl generated paths must be under FLATPAK_MUSL_WORK: $path" >&2; exit 2;;
  esac
done
mkdir -p "$HOST_CACHE" "$HOST_BUILD" "$HOST_OUT"
cache_inner_path() { printf '/scratch/%s' "${1#"$(realpath -m "$SCRATCH")"/}"; }
work_inner_path() { printf '/scratch/work/%s' "${1#"$(realpath -m "$WORK")"/}"; }
SCRATCH="$SCRATCH" FLATPAK_MUSL_WORK="$WORK" FLATPAK_CACHE="$HOST_CACHE" GPGME_LITE_SRC="$HOST_CACHE/src/DullPGP" \
  CACHE_DIR="$(cache_inner_path "$HOST_CACHE")" \
  BUILD_DIR="$(work_inner_path "$HOST_BUILD")" OUT_DIR="$(work_inner_path "$HOST_OUT")" \
  JOBS=${JOBS:-$(nproc)} START_AT=${START_AT:-all} \
  "$HERE/alpine-enter.sh" /work/build.sh
install -m755 "$HOST_OUT/matonos-flatpak" "$HERE/../prebuilt/static/x86_64/matonos-flatpak"
bash "$HERE/../../../tools/build-flatpak-helpers.sh"
{
  sed -E '/^[0-9a-f]{64}  matonos-flatpak$/d' "$HOST_OUT/SOURCE"
  echo
  echo 'static musl APK helper artifacts'
  (cd "$HERE/../prebuilt/static/x86_64" && sha256sum matonos-flatpak matonos-app-flatpak matonos-app-bwrap)
  awk '$2 != "matonos-app-bwrap"' "$HERE/../prebuilt/static/x86_64/HELPERS.sha256"
} > "$HOST_OUT/SOURCE.complete"

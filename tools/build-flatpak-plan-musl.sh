#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
SCRATCH=${MUSL_SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
WORK=$SCRATCH/flatpak-musl
PLAN_BUILD=${MATON_FLATPAK_PLAN_BUILD:-$WORK/build/flatpak}
PLAN_OUT=${MATON_FLATPAK_PLAN_OUT:-$WORK/plan}
GPGME_LITE_SRC=${GPGME_LITE_SRC:-$WORK/cache/src/DullPGP}
for path in "$PLAN_BUILD" "$GPGME_LITE_SRC"; do
  case "$(realpath -m "$path")" in
    "$(realpath -m "$SCRATCH")"/*) ;;
    *) echo "Flatpak plan musl inputs must be under MUSL_SCRATCH: $path" >&2; exit 2;;
  esac
done
inner_path() { printf '/scratch/%s' "${1#"$(realpath -m "$SCRATCH")"/}"; }
mkdir -p "$WORK/plan-source" "$WORK/plan-out" "$PLAN_OUT"
install -m644 "$ROOT/linux/subsystem/flatpak-plan/build.py" "$WORK/plan-source/build.py"
install -m644 "$ROOT/linux/subsystem/flatpak-plan/inspect.c" "$WORK/plan-source/inspect.c"
SCRATCH="$SCRATCH" GPGME_LITE_SRC="$GPGME_LITE_SRC" \
  MATON_FLATPAK_PLAN_BUILD="$(inner_path "$PLAN_BUILD")" \
  "$ROOT/linux/flatpak/build-support/alpine-enter.sh" \
  python3 /scratch/flatpak-musl/plan-source/build.py /scratch/flatpak-musl/plan-out
install -m755 "$WORK/plan-out/libmatonos-flatpak-plan.so" "$PLAN_OUT/libmatonos-flatpak-plan.so"
install -m755 "$WORK/plan-out/libmatonos-flatpak-plan.so.symbols" "$PLAN_OUT/libmatonos-flatpak-plan.so.symbols"
source_file=$ROOT/linux/flatpak/prebuilt/static/SOURCE
{
  awk '/^static musl plan helper artifact$/{exit} { line[NR]=$0 } END { while(NR>0 && line[NR]=="") NR--; for(i=1;i<=NR;i++) print line[i] }' "$source_file"
  echo
  echo 'static musl plan helper artifact'
  (cd "$PLAN_OUT" && sha256sum libmatonos-flatpak-plan.so)
} > "$WORK/SOURCE.plan"
install -m644 "$WORK/SOURCE.plan" "$source_file"

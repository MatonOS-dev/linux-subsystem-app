#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
SCRATCH=${MUSL_SCRATCH:-$HOME/matonos/build/musl-324}
WORK=${FLATPAK_MUSL_WORK:-$SCRATCH/flatpak-musl}
PLAN_BUILD=${MATON_FLATPAK_PLAN_BUILD:-$WORK/build/flatpak}
PLAN_OUT=${MATON_FLATPAK_PLAN_OUT:-$WORK/plan}
GPGME_LITE_SRC=${GPGME_LITE_SRC:-$SCRATCH/flatpak-musl/cache/src/DullPGP}
case "$(realpath -m "$PLAN_BUILD")" in
  "$(realpath -m "$WORK")"/*) ;;
  *) echo "Flatpak plan build must be under FLATPAK_MUSL_WORK: $PLAN_BUILD" >&2; exit 2;;
esac
for path in "$GPGME_LITE_SRC"; do
  case "$(realpath -m "$path")" in
    "$(realpath -m "$SCRATCH")"/*) ;;
    *) echo "Flatpak plan musl inputs must be under MUSL_SCRATCH: $path" >&2; exit 2;;
  esac
done
work_inner_path() { printf '/scratch/work/%s' "${1#"$(realpath -m "$WORK")"/}"; }
mkdir -p "$WORK/plan-source" "$WORK/plan-out" "$PLAN_OUT"
install -m644 "$ROOT/linux/subsystem/flatpak-plan/build.py" "$WORK/plan-source/build.py"
install -m644 "$ROOT/linux/subsystem/flatpak-plan/inspect.c" "$WORK/plan-source/inspect.c"
# The public inspection helper reuses Flatpak's static library link command.
# Its archive is only part of the Meson test target, not the CLI build above.
SCRATCH="$SCRATCH" FLATPAK_MUSL_WORK="$WORK" FLATPAK_CACHE="$SCRATCH/flatpak-musl/cache" \
  GPGME_LITE_SRC="$GPGME_LITE_SRC" \
  "$ROOT/linux/flatpak/build-support/alpine-enter.sh" \
  ninja -C "$(work_inner_path "$PLAN_BUILD")" common/libflatpak.a
SCRATCH="$SCRATCH" FLATPAK_MUSL_WORK="$WORK" FLATPAK_CACHE="$SCRATCH/flatpak-musl/cache" GPGME_LITE_SRC="$GPGME_LITE_SRC" \
  MATON_FLATPAK_PLAN_BUILD="$(work_inner_path "$PLAN_BUILD")" \
  "$ROOT/linux/flatpak/build-support/alpine-enter.sh" \
  python3 /scratch/work/plan-source/build.py /scratch/work/plan-out
install -m755 "$WORK/plan-out/libmatonos-flatpak-plan.so" "$PLAN_OUT/libmatonos-flatpak-plan.so"
install -m755 "$WORK/plan-out/libmatonos-flatpak-plan.so.symbols" "$PLAN_OUT/libmatonos-flatpak-plan.so.symbols"
source_file=$ROOT/linux/flatpak/prebuilt/static/SOURCE
{
  awk '/^static musl plan helper artifact$/{exit} { line[NR]=$0 } END { while(NR>0 && line[NR]=="") NR--; for(i=1;i<=NR;i++) print line[i] }' "$source_file"
  echo
  echo 'static musl plan helper artifact'
  (cd "$PLAN_OUT" && sha256sum libmatonos-flatpak-plan.so)
} > "$WORK/SOURCE.plan"
install -m644 "$WORK/SOURCE.plan" "$PLAN_OUT/SOURCE.plan"

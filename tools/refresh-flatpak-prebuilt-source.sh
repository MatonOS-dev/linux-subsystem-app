#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
: "${MATON_SUBSYSTEM_OUT:?set MATON_SUBSYSTEM_OUT to the completed build output directory}"
SCRATCH=${MATON_FLATPAK_SCRATCH:-$MATON_SUBSYSTEM_OUT/flatpak/scratch/flatpak-musl}
COMPLETE=${MATON_FLATPAK_OUT:-$SCRATCH/out}/SOURCE.complete
PLAN=${MATON_FLATPAK_PLAN_OUT:-$MATON_SUBSYSTEM_OUT/plan}/libmatonos-flatpak-plan.so
TARGET=$ROOT/linux/flatpak/prebuilt/static/SOURCE
[[ -s $COMPLETE ]] || { echo "Missing generated manifest: $COMPLETE" >&2; exit 2; }
[[ -s $PLAN ]] || { echo "Missing generated plan helper: $PLAN" >&2; exit 2; }
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
cat "$COMPLETE" > "$tmp"
printf '\nstatic musl plan helper artifact\n' >> "$tmp"
(cd "$(dirname "$PLAN")" && sha256sum "$(basename "$PLAN")") >> "$tmp"
install -m644 "$tmp" "$TARGET"
echo "Updated $TARGET from $MATON_SUBSYSTEM_OUT"

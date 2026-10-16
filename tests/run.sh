#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?output directory outside the repository required}
bash "$ROOT/linux/stubgen/tests/run-tests.sh" "${MATON_AOSP_ROOT:-/}" "$OUT/stubgen"
bash "$ROOT/linux/subsystem/host/test-host-command.sh" "$OUT/commands"
bash "$ROOT/linux/subsystem/host/test-portals.sh" "$OUT/portals"
bash "$ROOT/linux/packs/tests/run.sh"
python3 "$ROOT/linux/packs/tests/java-source.py"
python3 "$ROOT/linux/packs/tests/java-writer.py"
make -C "$ROOT/linux/dbus-broker" -j4 OUT="$OUT/bus" check-inhibit check-portal-wire check-policy-wildcard

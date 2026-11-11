#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-$PWD/build/apply-extra-host}
mkdir -p "$out"
# Under build/ so the linktest loader treats it as an app-data root.
mountpoint=$(mktemp -d "$PWD/build/apply-extra-noexec.XXXXXX")
trap 'rmdir "$mountpoint"' EXIT
unshare -Urnm sh -eu -c '
    mount --make-rprivate /
    mount -t tmpfs -o noexec tmpfs "$1"
    # Drop capabilities after mounting: Android apps lack CAP_DAC_OVERRIDE, so
    # opening a mode-01000 placeholder must fail exactly as it does on device.
    MATON_NOEXEC_DIR="$1" exec setpriv --bounding-set=-all --inh-caps=-all python3 "$2" "$3"
' sh "$mountpoint" "$PWD/tests/apply-extra-host.py" "$out"

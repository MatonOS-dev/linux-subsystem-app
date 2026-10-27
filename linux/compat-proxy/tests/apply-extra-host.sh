#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-$PWD/build/apply-extra-host}
mkdir -p "$out"
mountpoint=$(mktemp -d "$out/apply-extra-noexec.XXXXXX")
trap 'rmdir "$mountpoint"' EXIT
unshare -Urnm sh -eu -c '
    mount --make-rprivate /
    mount -t tmpfs -o noexec tmpfs "$1"
    MATON_NOEXEC_DIR="$1" python3 "$2" "$3"
' sh "$mountpoint" "$PWD/tests/apply-extra-host.py" "$out"

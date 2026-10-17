#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?external output directory required}
mkdir -p "$OUT"
javac -d "$OUT" "$ROOT/tests/workdirs/android/content/Context.java" "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/WorkDirs.java" "$ROOT/tests/workdirs/WorkDirsTest.java"
java -cp "$OUT" WorkDirsTest "$OUT"

#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?output directory required}
shift
mkdir -p "$OUT"
javac -d "$OUT" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/GuiProcess.java" \
  "$ROOT/linux/subsystem/host/app/src/test/java/org/matonos/linux/stub/GuiProcessTest.java"
java -cp "$OUT" org.matonos.linux.stub.GuiProcessTest "$@"

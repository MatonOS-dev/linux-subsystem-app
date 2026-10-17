#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?output directory required}
JAR=${MATON_ANDROID_JAR:-${MATON_ANDROID_SDK:-$HOME/Documents/matonos-android-sdk}/platforms/android-36/android.jar}
mkdir -p "$OUT"
APP=$ROOT/linux/subsystem/host/app/src/main/java
javac -d "$OUT" -cp "$JAR" "$APP/org/matonos/linux/stub/DeployManifest.java" \
    "$ROOT/linux/stubgen/src/org/matonos/linuxhost/stubgen/PlaceholderFiles.java" "$ROOT/tests/packagename/PackageNameTest.java"
java -cp "$OUT:$JAR" PackageNameTest

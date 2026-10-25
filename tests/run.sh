#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?output directory outside the repository required}
mkdir -p "$OUT"
python3 "$ROOT/tests/bootstrap-version-rule.py"
python3 "$ROOT/tests/cleanup-job-contract.py"
cc -O2 -Wall -Wextra -Werror "$ROOT/tests/short-socket-path.c" -o "$OUT/short-socket-path"
"$OUT/short-socket-path"
mkdir -p "$OUT/install-progress"
javac -d "$OUT/install-progress" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/InstallProgress.java" \
  "$ROOT/linux/subsystem/host/app/src/test/java/org/matonos/linux/stub/InstallProgressTest.java"
java -cp "$OUT/install-progress" org.matonos.linux.stub.InstallProgressTest
mkdir -p "$OUT/flatpak-output"
javac -d "$OUT/flatpak-output" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/FlatpakOutput.java" \
  "$ROOT/linux/subsystem/host/app/src/test/java/org/matonos/linux/stub/FlatpakOutputTest.java"
java -cp "$OUT/flatpak-output" org.matonos.linux.stub.FlatpakOutputTest
bash "$ROOT/tests/workdirs/run.sh" "$OUT/workdirs"
bash "$ROOT/linux/stubgen/tests/run-tests.sh" "${MATON_AOSP_ROOT:-/}" "$OUT/stubgen"
bash "$ROOT/tests/placeholder/run.sh" "$OUT/placeholder"
bash "$ROOT/linux/subsystem/host/test-host-command.sh" "$OUT/commands"
bash "$ROOT/linux/subsystem/host/test-portals.sh" "$OUT/portals"
bash "$ROOT/linux/packs/tests/run.sh"
python3 "$ROOT/linux/packs/tests/java-source.py"
python3 "$ROOT/linux/packs/tests/java-writer.py"
bash "$ROOT/linux/glx-forward/tests/run-host.sh" "$OUT/glx-forward"
bash "$ROOT/linux/gl-maton/tests/run.sh" "$OUT/gl-maton"
make -C "$ROOT/linux/dbus-broker" -j4 OUT="$OUT/bus" check-inhibit check-portal-wire check-policy-wildcard
bash "$ROOT/tests/packagename/run.sh" "$OUT/packagename"
mkdir -p "$OUT/install-handoff/classes"
ANDROID_JAR=${MATON_ANDROID_JAR:-/mnt/data/aosp/prebuilts/sdk/current/public/android.jar}
JSON_JAR=${MATON_JSON_JAR:-/mnt/data/aosp/prebuilts/misc/common/json/json-prebuilt.jar}
AAPT2=${MATON_AAPT2:-/mnt/data/aosp/out/host/linux-x86/bin/aapt2}
javac -cp "$ANDROID_JAR:$JSON_JAR:$ROOT/linux/stubgen/prebuilt/apksig.jar" -d "$OUT/install-handoff/classes" \
  "$ROOT"/linux/stubgen/src/org/matonos/linuxhost/stubgen/*.java \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/DeployManifest.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/DeployRecord.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/InstallHandoff.java" \
  "$ROOT/tests/install-handoff/InstallHandoffTest.java"
bash "$ROOT/linux/stubgen/tests/build-loader-dex.sh" "$ANDROID_JAR" "${MATON_D8:-$(dirname "$AAPT2")/d8}" "$OUT/install-handoff/classes"
java -cp "$OUT/install-handoff/classes:$JSON_JAR:$ANDROID_JAR:$ROOT/linux/stubgen/prebuilt/apksig.jar" \
  org.matonos.linuxhost.stubgen.InstallHandoffTest "$OUT/install-handoff"
"$AAPT2" dump badging "$OUT/install-handoff/handoff.apk" | grep -F "package: name='flatpak.app.org_example_Editor.x86_64.stable.x" >/dev/null

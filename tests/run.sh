#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?output directory outside the repository required}
mkdir -p "$OUT"
bash "$ROOT/tests/gui-process.sh" "$OUT/gui-process"
python3 "$ROOT/tests/bootstrap-version-rule.py"
python3 "$ROOT/tests/cleanup-job-contract.py"
python3 "$ROOT/linux/subsystem/native/tests/presenter_backpressure_test.py"
cc -O2 -Wall -Wextra -Werror "$ROOT/linux/subsystem/native/tests/presenter_state_test.c" -o "$OUT/presenter-state-test"
"$OUT/presenter-state-test"
cc -O2 -Wall -Wextra -Werror "$ROOT/tests/short-socket-path.c" -o "$OUT/short-socket-path"
"$OUT/short-socket-path"
mkdir -p "$OUT/install-progress"
javac -d "$OUT/install-progress" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/InstallProgress.java" \
  "$ROOT/linux/subsystem/host/app/src/test/java/org/matonos/linux/stub/InstallProgressTest.java"
java -cp "$OUT/install-progress" org.matonos.linux.stub.InstallProgressTest
mkdir -p "$OUT/stub-session"
javac -d "$OUT/stub-session" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/WindowLifetime.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/LoaderQuit.java" \
  "$ROOT/linux/subsystem/host/app/src/test/java/org/matonos/linux/stub/WindowLifetimeTest.java" \
  "$ROOT/linux/subsystem/host/app/src/test/java/org/matonos/linux/stub/LoaderQuitTest.java"
java -cp "$OUT/stub-session" org.matonos.linux.stub.WindowLifetimeTest
java -cp "$OUT/stub-session" org.matonos.linux.stub.LoaderQuitTest
mkdir -p "$OUT/chrome-per-app-dirs/classes" "$OUT/chrome-per-app-dirs/runtime"
javac -d "$OUT/chrome-per-app-dirs/classes" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/FlatpakPerAppDirs.java" \
  "$ROOT/tests/chrome-sol/FlatpakPerAppDirsTest.java"
java -cp "$OUT/chrome-per-app-dirs/classes" FlatpakPerAppDirsTest \
  "$OUT/chrome-per-app-dirs/runtime"
mkdir -p "$OUT/machine-id/classes" "$OUT/machine-id/files"
javac -d "$OUT/machine-id/classes" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/MachineId.java" \
  "$ROOT/linux/subsystem/host/app/src/test/java/org/matonos/linux/stub/MachineIdTest.java"
java -cp "$OUT/machine-id/classes" org.matonos.linux.stub.MachineIdTest "$OUT/machine-id/files"
mkdir -p "$OUT/flatpak-output"
javac -d "$OUT/flatpak-output" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/FlatpakOutput.java" \
  "$ROOT/linux/subsystem/host/app/src/test/java/org/matonos/linux/stub/FlatpakOutputTest.java"
java -cp "$OUT/flatpak-output" org.matonos.linux.stub.FlatpakOutputTest
mkdir -p "$OUT/extra-data-files"
javac -d "$OUT/extra-data-files" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/ExtraDataFiles.java" \
  "$ROOT/tests/extra-data-files/ExtraDataFilesTest.java"
java -cp "$OUT/extra-data-files" org.matonos.linux.stub.ExtraDataFilesTest
mkdir -p "$OUT/extra-data-handoff"
javac -d "$OUT/extra-data-handoff" \
  "$ROOT/linux/stubgen/src/org/matonos/linuxhost/stubgen/PlaceholderFiles.java" \
  "$ROOT/linux/stubgen/src/org/matonos/linuxhost/stubgen/ObjectPacks.java" \
  "$ROOT/linux/stubgen/src/org/matonos/linuxhost/stubgen/PackLaunch.java" \
  "$ROOT/linux/stubgen/src/org/matonos/linuxhost/stubgen/ErofsImage.java" \
  "$ROOT/linux/stubgen/src/org/matonos/linuxhost/stubgen/OstreePacks.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/ExtraDataImage.java" \
  "$ROOT/tests/extra-data-handoff/ExtraDataImageTest.java"
java -cp "$OUT/extra-data-handoff" org.matonos.linux.stub.ExtraDataImageTest
mkdir -p "$OUT/extra-image-validator"
javac -d "$OUT/extra-image-validator" \
  "$ROOT/linux/stubgen/src/org/matonos/linuxhost/stubgen/PlaceholderFiles.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/ExtraImageValidator.java" \
  "$ROOT/tests/extra-image-validator/ExtraImageValidatorTest.java"
java -cp "$OUT/extra-image-validator" org.matonos.linux.stub.ExtraImageValidatorTest
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
make -C "$ROOT/linux/dbus-broker" -j"$(nproc)" OUT="$OUT/bus" check-inhibit check-portal-wire check-policy-wildcard
bash "$ROOT/tests/packagename/run.sh" "$OUT/packagename"
mkdir -p "$OUT/install-handoff/classes"
ANDROID_JAR=${MATON_ANDROID_JAR:-/mnt/data/aosp/prebuilts/sdk/current/public/android.jar}
JSON_JAR=${MATON_JSON_JAR:-/mnt/data/aosp/prebuilts/misc/common/json/json-prebuilt.jar}
AAPT2=${MATON_AAPT2:-/mnt/data/aosp/out/host/linux-x86/bin/aapt2}
javac -cp "$ANDROID_JAR:$JSON_JAR:$ROOT/linux/stubgen/prebuilt/apksig.jar" -d "$OUT/install-handoff/classes" \
  "$ROOT"/linux/stubgen/src/org/matonos/linuxhost/stubgen/*.java \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/DeployManifest.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/PackageMounts.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/DeployRecord.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/InstallHandoff.java" \
  "$ROOT/tests/install-handoff/InstallHandoffTest.java" \
  "$ROOT/tests/install-handoff/PackageMountsPlanTest.java"
bash "$ROOT/linux/stubgen/tests/build-loader-dex.sh" "$ANDROID_JAR" "${MATON_D8:-$(dirname "$AAPT2")/d8}" "$OUT/install-handoff/classes"
rm -rf "$OUT/install-handoff/fixture"
java -cp "$OUT/install-handoff/classes:$JSON_JAR:$ANDROID_JAR:$ROOT/linux/stubgen/prebuilt/apksig.jar" \
  org.matonos.linuxhost.stubgen.InstallHandoffTest "$OUT/install-handoff/fixture"
java -cp "$OUT/install-handoff/classes:$JSON_JAR:$ANDROID_JAR:$ROOT/linux/stubgen/prebuilt/apksig.jar" \
  org.matonos.linux.stub.PackageMountsPlanTest
"$AAPT2" dump badging "$OUT/install-handoff/fixture/handoff.apk" | grep -F "package: name='flatpak.app.org_example_Editor.x86_64.stable.x" >/dev/null
if [[ ${MATON_HOST_SMOKE:-0} == 1 ]]; then
  MATON_HOST_SMOKE_HEADLESS_ONLY=1 bash "$ROOT/tests/host-smoke/run.sh" \
    net.sourceforge.ExtremeTuxRacer org.gnome.TextEditor
fi

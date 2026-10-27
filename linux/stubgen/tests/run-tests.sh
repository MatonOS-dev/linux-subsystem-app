#!/usr/bin/env bash
# Usage: bash run-tests.sh AOSP_ROOT OUTPUT_DIRECTORY (outside source projects)
set -euo pipefail
AOSP_ROOT=$(readlink -m "$1")
ANDROID_JAR=${MATON_ANDROID_JAR:-$AOSP_ROOT/prebuilts/sdk/current/public/android.jar}
AAPT2=${MATON_AAPT2:-$AOSP_ROOT/out/host/linux-x86/bin/aapt2}
TEST_OUT=$(readlink -m "$2")
STUB_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
mkdir -p "$TEST_OUT/classes"
openssl req -x509 -newkey rsa:2048 -keyout "$TEST_OUT/key.pem" -out "$TEST_OUT/cert.pem" -nodes -subj /CN=DisposableImageTest -days 1 >"$TEST_OUT/keygen.log" 2>&1
openssl pkcs8 -topk8 -nocrypt -in "$TEST_OUT/key.pem" -outform DER -out "$TEST_OUT/key.der"
trap 'rm -f "$TEST_OUT/key.pem" "$TEST_OUT/key.der"' EXIT
bash "$STUB_DIR/tests/build-loader-dex.sh" "$ANDROID_JAR" "${MATON_D8:-$(dirname "$AAPT2")/d8}" "$TEST_OUT/classes"
javac -cp "$ANDROID_JAR:$STUB_DIR/prebuilt/apksig.jar" -d "$TEST_OUT/classes" "$STUB_DIR"/src/org/matonos/linuxhost/stubgen/*.java "$STUB_DIR"/tests/*.java \
  "$STUB_DIR/../subsystem/host/app/src/main/java/org/matonos/linux/stub/PackInstaller.java"
java -Xmx256m -cp "$TEST_OUT/classes" DependencyPlanTest
java -Xmx256m -cp "$TEST_OUT/classes" org.matonos.linuxhost.stubgen.PlanExecutorTest
java -Xmx256m -cp "$TEST_OUT/classes" PackageGraphTest
java -Xmx256m -cp "$TEST_OUT/classes" DeploymentRecordTest "$TEST_OUT"
if command -v flatpak >/dev/null && command -v ostree >/dev/null && \
    python3 -c 'from gi.repository import GLib' >/dev/null 2>&1; then
  python3 "$STUB_DIR/tests/deployment-flatpak-test.py" "$TEST_OUT/classes" "$TEST_OUT/flatpak-lookup"
else
  echo 'SKIP stock Flatpak deployment lookup test (requires Flatpak, OSTree and PyGObject)'
fi
java -Xmx256m -cp "$TEST_OUT/classes" PackSessionPlanTest
java -Xmx256m -cp "$TEST_OUT/classes:$ANDROID_JAR:$STUB_DIR/prebuilt/apksig.jar" \
  org.matonos.linuxhost.stubgen.SessionPlanHandoffTest "$TEST_OUT" "$TEST_OUT/key.der" "$TEST_OUT/cert.pem"
java -Xmx256m -cp "$TEST_OUT/classes" ControllerMetadataTest
java -Xmx256m -cp "$TEST_OUT/classes" CommitValidationTest
java -cp "$TEST_OUT/classes:$ANDROID_JAR" ForegroundManifestTest "$AAPT2" "$TEST_OUT"

java -cp "$TEST_OUT/classes:$STUB_DIR/prebuilt/apksig.jar:$ANDROID_JAR" PackCarrierTest "$TEST_OUT" "$AAPT2"
java -Xmx256m -cp "$TEST_OUT/classes" PackSeedTest "$(mktemp -d "$TEST_OUT/seed.XXXXXX")"
java -Xmx256m -cp "$TEST_OUT/classes" SubpathPackTest "$(mktemp -d "$TEST_OUT/subpath.XXXXXX")"

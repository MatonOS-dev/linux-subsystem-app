#!/usr/bin/env bash
# Usage: bash run-tests.sh AOSP_ROOT OUTPUT_DIRECTORY (outside source projects)
set -euo pipefail
AOSP_ROOT=$(readlink -f "$1")
TEST_OUT=$(readlink -m "$2")
STUB_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
mkdir -p "$TEST_OUT/classes"
openssl req -x509 -newkey rsa:2048 -keyout "$TEST_OUT/key.pem" -out "$TEST_OUT/cert.pem" -nodes -subj /CN=DisposableImageTest -days 1 >"$TEST_OUT/keygen.log" 2>&1
openssl pkcs8 -topk8 -nocrypt -in "$TEST_OUT/key.pem" -outform DER -out "$TEST_OUT/key.der"
trap 'rm -f "$TEST_OUT/key.pem" "$TEST_OUT/key.der"' EXIT
javac -cp "$AOSP_ROOT/prebuilts/sdk/current/public/android.jar:$STUB_DIR/prebuilt/apksig.jar" -d "$TEST_OUT/classes" "$STUB_DIR/src/org/matonos/linuxhost/stubgen/StubGenerator.java" "$STUB_DIR"/tests/*.java
java -Xmx256m -cp "$TEST_OUT/classes" ControllerMetadataTest
java -Xmx256m -cp "$TEST_OUT/classes" CommitValidationTest
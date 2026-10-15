#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: test-portals.sh OUTPUT_DIRECTORY}
mkdir -p "$OUT"
LIBS="$ROOT/app/libs/*"
javac -cp "$LIBS" -d "$OUT" "$ROOT/app/src/main/java/org/matonos/linux/DBusSignature.java" \
    "$ROOT/app/src/main/java/org/matonos/linux/DBusReader.java" \
    "$ROOT/app/src/main/java/org/matonos/linux/DBusWriter.java" \
    "$ROOT/app/src/main/java/org/matonos/linux/PortalWire.java" \
    "$ROOT/app/src/main/java/org/matonos/linux/PortalChannel.java" \
    "$ROOT/app/src/main/java/org/matonos/linux/PeerConnection.java" \
    "$ROOT/app/src/main/java/org/matonos/linux/PortalBackend.java" \
    "$ROOT/app/src/main/java/org/matonos/linux/PortalIntrospection.java" \
    "$ROOT/app/src/test/java/org/matonos/linux/PipeChannel.java" \
    "$ROOT/app/src/test/java/org/matonos/linux/PortalTestPeer.java" \
    "$ROOT/app/src/test/java/org/matonos/linux/PortalBackendTest.java" \
    "$ROOT/app/src/test/java/org/matonos/linux/PortalConnectionTest.java"
java -cp "$OUT:$LIBS" org.matonos.linux.PortalBackendTest
java -cp "$OUT:$LIBS" org.matonos.linux.PortalConnectionTest

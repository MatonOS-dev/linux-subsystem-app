#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
output=${1:?usage: test-host-command.sh OUTPUT_DIRECTORY}
mkdir -p "$output"
javac -d "$output" "$root"/app/src/main/java/org/matonos/linux/{DBusSignature,DBusReader,DBusWriter,PortalWire,HostCommandBackend}.java \
    "$root/app/src/test/java/org/matonos/linux/HostCommandBackendTest.java"
java -cp "$output" org.matonos.linux.HostCommandBackendTest

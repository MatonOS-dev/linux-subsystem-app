#!/usr/bin/env bash
# Usage: build-loader-dex.sh ANDROID_JAR D8 CLASSES_DIR
# Puts the stub loader dex where StubGenerator.loaderDex() finds it on the classpath.
set -euo pipefail
jar=$1 d8=$2 out=$3
shared=$(dirname "$(readlink -f "$0")")/../../subsystem/host/shared/src/main/java
work=$(mktemp -d "$out/.loader.XXXXXX")
trap 'rm -rf "$work"' EXIT
javac --release 17 -cp "$jar" -d "$work" $(find "$shared" -name '*.java')
"$d8" --release --min-api 30 --lib "$jar" --output "$work" $(find "$work" -name '*.class')
mkdir -p "$out/org/matonos/linuxhost/stubgen"
mv "$work/classes.dex" "$out/org/matonos/linuxhost/stubgen/stub-loader.dex"

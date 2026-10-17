#!/usr/bin/env bash
# Java-side placeholder resolver tests.
#
#   PlaceholderFilesTest   host unit tests for the detection/read semantics.
#   PlaceholderFixtureTest reads a real compatibility-proxy staging tree built by
#                          ostree init/commit/checkout --require-hardlinks inside
#                          a marked store and proves content, size and checksum
#                          parity with a copy-mode checkout and an identical pack
#                          writer output.
#
# Usage: bash tests/placeholder/run.sh OUTPUT_DIRECTORY
set -Eeuo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?output directory outside the repository required}
AOSP_ROOT=$(readlink -m "${MATON_AOSP_ROOT:-/mnt/data/aosp}")
CLASSES=$OUT/classes
mkdir -p "$CLASSES"

# Static regression guard: every staging read must route through PlaceholderFiles.
python3 "$ROOT/tests/placeholder/guard.py" "$ROOT"

# First existing path wins; the build's MATON_AOSP_ROOT may be a bare context
# directory, so the real AOSP prebuilts are a fallback.
resolve_jar() {
    local candidate
    for candidate in "$@"; do
        [[ -n $candidate && -f $candidate ]] && { echo "$candidate"; return 0; }
    done
    return 1
}
ANDROID_JAR=$(resolve_jar "${MATON_ANDROID_JAR:-}" \
    "$AOSP_ROOT/prebuilts/sdk/current/public/android.jar" \
    "${ANDROID_HOME:-}/platforms/android-36/android.jar" \
    "${ANDROID_SDK_ROOT:-}/platforms/android-36/android.jar" \
    /mnt/data/aosp/prebuilts/sdk/current/public/android.jar) \
    || { echo "missing android.jar; set MATON_ANDROID_JAR" >&2; exit 1; }
JSON_JAR=$(resolve_jar "${MATON_JSON_JAR:-}" \
    "$AOSP_ROOT/prebuilts/misc/common/json/json-prebuilt.jar" \
    /mnt/data/aosp/prebuilts/misc/common/json/json-prebuilt.jar) \
    || { echo "missing a real org.json; set MATON_JSON_JAR" >&2; exit 1; }
APKSIG_JAR=${MATON_APKSIG_JAR:-$ROOT/linux/stubgen/prebuilt/apksig.jar}
[[ -f $APKSIG_JAR ]] || { echo "missing apksig.jar at $APKSIG_JAR" >&2; exit 1; }

# The fixture class lives in the subsystem stub package so it can call the
# package-private DeployManifest entry points.
javac -cp "$ANDROID_JAR:$APKSIG_JAR" -d "$CLASSES" \
    "$ROOT"/linux/stubgen/src/org/matonos/linuxhost/stubgen/*.java \
    "$ROOT"/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/DeployManifest.java \
    "$ROOT"/tests/placeholder/PlaceholderFilesTest.java \
    "$ROOT"/tests/placeholder/PlaceholderFixtureTest.java

java -Xmx256m -cp "$CLASSES" PlaceholderFilesTest

if ! command -v ostree >/dev/null 2>&1 || ! command -v cc >/dev/null 2>&1; then
    echo "SKIP placeholder proxy fixture (host ostree and cc required)"
    exit 0
fi

# Build the real proxy freestanding x86_64 layer from a source copy outside the
# repository. The fixture only needs that layer; the i386 peer is irrelevant.
PROXY=$OUT/proxy/linux/compat-proxy
rm -rf "$OUT/proxy"
mkdir -p "$OUT/proxy/linux"
cp -r "$ROOT/linux/compat-proxy" "$PROXY"
cp -r "$ROOT/linux/packs" "$OUT/proxy/linux/packs"
(
    cd "$PROXY"
    python3 gen-policy.py
    mkdir -p build
    chmod 0755 build
    cc -nostdlib -static -no-pie -mcmodel=large \
        -Wl,-Ttext-segment=0x600000000000 -Wl,--section-start=.proxy_gateway=0x600000300000 \
        -O2 -fno-stack-protector -fno-builtin -Wall -Wextra -Werror \
        loader.c proxy.c vfs.c links.c net.c sysvipc.c state.c arch/x86_64/raw.S arch/x86_64/entry.S \
        -o build/linux-layer-v3
    chmod 0755 build/linux-layer-v3
    cc -no-pie -O2 -Wall -Wextra -Werror tests/link-deny.c -o build/link-deny
    cc -no-pie -O2 -Wall -Wextra -Werror "$ROOT/tests/placeholder/link-helper.c" -o build/link-helper
)

PROXY_BIN=$PROXY/build/linux-layer-v3
DENY=$PROXY/build/link-deny
LINK=$PROXY/build/link-helper
OSTREE=$(command -v ostree)

FIXTURE=$OUT/fixture
STAGE=$FIXTURE/stage            # marked store root == Flatpak installation
COPYROOT=$FIXTURE/copy          # copy-mode checkout, outside the store
rm -rf "$FIXTURE"
mkdir -m 700 "$FIXTURE"
mkdir -m 700 "$STAGE"
touch "$STAGE/.v3-store-root"

# Run a host command through the real proxy with native linkat denied, so the
# proxy alone decides what a hardlink means.
proxy() { timeout 120 "$DENY" "$PROXY_BIN" --android-filter "$1" "${@:2}"; }
ostree_run() { proxy "$OSTREE" --repo="$STAGE/repo" "$@"; }

mkdir -p "$STAGE/input/sub"
printf 'metadata content\n' >"$STAGE/input/metadata"
printf 'hello world hello world\n' >"$STAGE/input/data.txt"
printf '#!/bin/sh\necho fixture\n' >"$STAGE/input/run.sh"
chmod 755 "$STAGE/input/run.sh"
printf 'nested payload bytes\n' >"$STAGE/input/sub/nested.bin"
ln -s data.txt "$STAGE/input/link"

ostree_run init --mode=bare-user-only
ostree_run commit --branch=fixture --subject=fixture --tree="dir=$STAGE/input"
COMMIT=$(ostree_run rev-parse fixture | tr -d '[:space:]')
REMOTE=fixture-remote
APP_REF=app/org.example.Fixture/x86_64/stable

DEPLOY=$STAGE/$APP_REF/$COMMIT
mkdir -p "$DEPLOY" "$STAGE/seed"
ostree_run checkout --user-mode --require-hardlinks fixture "$DEPLOY/files"
ostree_run checkout --user-mode fixture "$COPYROOT"

# deploy is the Flatpak (ssasta{sv}) identity; origin and commit must agree with
# the remote and the directory name. Writing it and metadata as hardlinks of
# seed files makes the proxy store them as indexed placeholders too.
python3 - "$REMOTE" "$COMMIT" "$STAGE/seed/deploy" <<'PY'
import sys
origin, commit, out = sys.argv[1], sys.argv[2], sys.argv[3]
data = origin.encode() + b'\0' + commit.encode() + b'\0'
data += b'\0' * ((-len(data)) % 8)
data += (1).to_bytes(8, 'little')
open(out, 'wb').write(data)
PY
cp "$STAGE/input/metadata" "$STAGE/seed/metadata"
proxy "$LINK" "$STAGE/seed/deploy" "$DEPLOY/deploy"
proxy "$LINK" "$STAGE/seed/metadata" "$DEPLOY/metadata"

java -Xmx256m -cp "$CLASSES:$JSON_JAR:$APKSIG_JAR:$ANDROID_JAR" \
    org.matonos.linux.stub.PlaceholderFixtureTest \
    "$STAGE" "$COPYROOT" "$REMOTE" "$APP_REF" "$COMMIT" "$OUT/packs"

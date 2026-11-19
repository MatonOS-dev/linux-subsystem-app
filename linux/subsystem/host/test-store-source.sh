#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
output=${1:?usage: test-store-source.sh OUTPUT_DIRECTORY}
mkdir -p "$output"
javac -d "$output" "$root/app/src/main/java/org/matonos/linux/store/AppStreamCatalogue.java" \
    "$root/app/src/main/java/org/matonos/linux/store/FlathubBetaPolicy.java" \
    "$root/app/src/main/java/org/matonos/linux/store/RemoteAppStream.java" \
    "$root/app/src/main/java/org/matonos/linux/store/CommitChain.java" \
    "$root"/app/src/main/java/org/matonos/linux/store/{SourcePaging,SourceCandidates,SourceOperations,StoreRepair}.java \
    "$root/app/src/test/java/org/matonos/linux/StoreSourceHostTest.java"
java -cp "$output" org.matonos.linux.StoreSourceHostTest

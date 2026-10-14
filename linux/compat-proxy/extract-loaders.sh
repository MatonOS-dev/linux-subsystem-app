#!/usr/bin/env bash
# Extract trusted APK copies for the image's executable loader staging.
# Called only after build-apps.sh has verified the host APK signature.
set -euo pipefail
if (( $# != 2 )); then echo 'usage: extract-loaders.sh VERIFIED_HOST_APK OUTPUT_DIRECTORY' >&2; exit 2; fi
apk=$1
output=$2
mkdir -p "$output"
for name in libmatonos-v3-loader.so libmatonos-v3-loader-i386.so; do
    staged=$(mktemp "$output/.$name.XXXXXX")
    trap 'rm -f "$staged"' EXIT
    unzip -p "$apk" "lib/x86_64/$name" > "$staged"
    [[ -s $staged ]] || { echo "host APK has no $name" >&2; exit 1; }
    chmod 0755 "$staged"
    mv -f "$staged" "$output/$name"
    trap - EXIT
done

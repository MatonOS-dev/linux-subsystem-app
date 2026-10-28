#!/usr/bin/env bash
# Extract trusted APK copies for the image's executable helper staging.
# Called only after build-apps.sh has verified the host APK signature.
set -euo pipefail
if (( $# != 2 )); then echo 'usage: extract-loaders.sh VERIFIED_HOST_APK OUTPUT_DIRECTORY' >&2; exit 2; fi
apk=$1
output=$2
mkdir -p "$output"
# System APKs do not receive PM extraction. Keep every APK native library in
# the same app-local directory, including executable tools and Xwayland DSOs.
while IFS= read -r entry; do
    name=${entry##*/}
    [[ $name == lib*.so ]] || continue
    staged=$(mktemp "$output/.$name.XXXXXX")
    trap 'rm -f "$staged"' EXIT
    unzip -p "$apk" "$entry" > "$staged"
    [[ -s $staged ]] || { echo "host APK has no $name" >&2; exit 1; }
    chmod 0755 "$staged"
    mv -f "$staged" "$output/$name"
    trap - EXIT
done < <(unzip -Z1 "$apk" | sed -n '\|^lib/x86_64/lib[^/]*\.so$|p')
# Both ELF classes are required for cross-class exec. Reject incomplete APKs
# even if another native library was extracted successfully. The static x86_64
# peer (i386-to-x86_64 execs) is required too; without it those execs cannot
# hand off at all.
for name in libmatonos-v3-peer-i386.so libmatonos-v3-loader.so libmatonos-v3-loader-i386.so libmatonos-v3-loader-static.so; do
    unzip -Z1 "$apk" | grep -Fx "lib/x86_64/$name" >/dev/null || {
        echo "host APK has no $name" >&2
        exit 1
    }
done

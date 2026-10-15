#!/bin/sh
# Sync the in-tree helper sources that MatonOS_apexs builds into its flatpak/helpers tree and check they build there with
# the pinned NDK (bionic; the Alpine/musl recipe is retired), and install the
# two binaries into prebuilt/static/<arch>. flatpak-env-wrapper and
# matonos-app-tool are built in-tree by tools/build-flatpak-helpers.sh; the
# Flatpak multicall, portal and SOURCE come from MatonOS_apexs flatpak/build.sh.
# Commit the synced helpers in MatonOS_apexs together with this tree.
set -eu

TREE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
APEX_REPO=${MATONOS_APEXS_REPO:-$HOME/matonos/repos/MatonOS_apexs}
ARCH=${ARCH:-x86_64}
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/30.0.16248370}
branch=$(git -C "$TREE" branch --show-current)
[ "$branch" = main ] || {
    echo "ERROR: run stage-apex-helpers.sh from the main worktree (found ${branch:-detached})" >&2; exit 2;
}
[ -d "$APEX_REPO/.git" ] || { echo "Missing MatonOS_apexs repo: $APEX_REPO" >&2; exit 2; }
[ -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ] || { echo "Missing NDK: $NDK" >&2; exit 2; }

helpers="$APEX_REPO/flatpak/helpers"
sync() { mkdir -p "$(dirname "$helpers/$2")"; install -m644 "$TREE/$1" "$helpers/$2"; }
# Management helper sources and the shared canonical path header.
sync linux/flatpak/linux-paths.h linux-paths.h
sync linux/flatpak/linux-paths.h linux/flatpak/linux-paths.h
for f in SafePath.h FlatpakPublish.h; do
    sync "install/linuxd/$f" "install/linuxd/$f"
done
(
    cd "$APEX_REPO/flatpak"
    find helpers -type f ! -name SOURCE.sha256 -print0 | sort -z | xargs -0 sha256sum
) > "$helpers/SOURCE.sha256"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
ANDROID_NDK=$NDK OUT_DIR=$out sh "$APEX_REPO/flatpak/build-helpers.sh"

# The device tree's prebuilt comes from tools/build-flatpak-helpers.sh (it
# owns HELPERS.sha256); this build only proves the synced sources compile in
# MatonOS_apexs for its own provenance manifest.

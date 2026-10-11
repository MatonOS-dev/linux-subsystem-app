#!/bin/sh
# Sync the in-tree helper sources that MatonOS_apexs builds (matonos-app-exec,
# matonos-flatpak-store) into its flatpak/helpers tree, build them there with
# the pinned NDK (bionic; the Alpine/musl recipe is retired), and install the
# two binaries into prebuilt/static/<arch>. flatpak-env-wrapper and
# matonos-bwrap are built in-tree by tools/build-flatpak-helpers.sh; the
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
# Sources and every header they include, at the include paths they use.
sync linux/flatpak/matonos-bwrap.c matonos-bwrap.c
sync linux/flatpak/matonos-app-exec.c matonos-app-exec.c
sync install/linuxd/MatonMls.h MatonMls.h
sync linux/flatpak/linux-paths.h linux-paths.h
sync linux/flatpak/controller-access.h controller-access.h
for f in flatpak-env-wrapper.c machine-id.h linux-paths.h app-session.h session-binder-filter.h socket-relay.h; do
    sync "linux/flatpak/$f" "linux/flatpak/$f"
done
sync linux/dbus-broker/session-control.h linux/dbus-broker/session-control.h
for f in DownloadsGrant.h DownloadsUserDirs.h GtkSettings.h SafePath.h MatonMls.h FlatpakStore.c FlatpakStore.h; do
    sync "install/linuxd/$f" "install/linuxd/$f"
done
(
    cd "$APEX_REPO/flatpak"
    find helpers -type f ! -name SOURCE.sha256 -print0 | sort -z | xargs -0 sha256sum
) > "$helpers/SOURCE.sha256"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
ANDROID_NDK=$NDK OUT_DIR=$out sh "$APEX_REPO/flatpak/build-helpers.sh"

stage="$TREE/linux/flatpak/prebuilt/static/$ARCH"
for name in matonos-app-exec matonos-flatpak-store; do
    install -m755 "$out/$name" "$stage/$name"
    printf '%s %s bytes\n' "$name" "$(wc -c < "$stage/$name" | tr -d ' ')"
done

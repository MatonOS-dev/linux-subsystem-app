#!/bin/sh
set -eu

TREE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
APEX_REPO=${MATONOS_APEXS_REPO:-$HOME/matonos/repos/MatonOS_apexs}
ARCH=${ARCH:-$(uname -m)}
[ "$(git -C "$TREE" branch --show-current)" = static-flatpak-apex ] || {
    echo "ERROR: expected static-flatpak-apex worktree" >&2; exit 2;
}
[ -d "$APEX_REPO/.git" ] || { echo "Missing MatonOS_apexs repo: $APEX_REPO" >&2; exit 2; }

helpers="$APEX_REPO/flatpak/helpers"
mkdir -p "$helpers"
install -m644 "$TREE/linux/flatpak/matonos-bwrap.c" "$helpers/matonos-bwrap.c"
install -m644 "$TREE/linux/flatpak/matonos-app-exec.c" "$helpers/matonos-app-exec.c"
install -m644 "$TREE/install/linuxd/MatonMls.h" "$helpers/MatonMls.h"
install -m644 "$TREE/linux/flatpak/controller-access.h" "$helpers/controller-access.h"
(cd "$APEX_REPO/flatpak" && sha256sum helpers/matonos-bwrap.c \
    helpers/matonos-app-exec.c helpers/MatonMls.h helpers/controller-access.h) \
    > "$helpers/SOURCE.sha256"

ARCH=$ARCH HELPERS_ONLY=1 "$APEX_REPO/flatpak/alpine-enter.sh" /work/build-helpers.sh

out=/mnt/data/aosp/out/pc-logs/musl-324/output
stage="$TREE/linux/flatpak/prebuilt/static/$ARCH"
mkdir -p "$stage"
for name in matonos-flatpak matonos-bwrap matonos-app-exec; do
    install -m755 "$out/$name" "$stage/$name"
done

source_file="$TREE/linux/flatpak/prebuilt/static/SOURCE"
awk '/^matonos-bwrap$/ { exit } { print }' "$source_file" > "$source_file.tmp"
{
    cat "$source_file.tmp"
    printf 'matonos-bwrap\n  SHA-256: %s\n  Source: linux/flatpak/matonos-bwrap.c\n  Source SHA-256: %s\n  Header: linux/flatpak/controller-access.h\n  Header SHA-256: %s\n  Toolchain: Alpine 3.24 musl, cc, -Os -ffunction-sections -fdata-sections -static-pie -Wl,--gc-sections; strip --strip-all\n\n' \
        "$(sha256sum "$stage/matonos-bwrap" | awk '{print $1}')" \
        "$(sha256sum "$TREE/linux/flatpak/matonos-bwrap.c" | awk '{print $1}')" \
        "$(sha256sum "$TREE/linux/flatpak/controller-access.h" | awk '{print $1}')"
    printf 'matonos-app-exec\n  SHA-256: %s\n  Source: linux/flatpak/matonos-app-exec.c and install/linuxd/MatonMls.h\n  Source SHA-256: %s\n  Header SHA-256: %s\n  Toolchain: Alpine 3.24 musl, cc, -Os -ffunction-sections -fdata-sections -static-pie -Wl,--gc-sections; strip --strip-all\n' \
        "$(sha256sum "$stage/matonos-app-exec" | awk '{print $1}')" \
        "$(sha256sum "$TREE/linux/flatpak/matonos-app-exec.c" | awk '{print $1}')" \
        "$(sha256sum "$TREE/install/linuxd/MatonMls.h" | awk '{print $1}')"
} > "$source_file"
rm -f "$source_file.tmp"
"$APEX_REPO/flatpak/check-static-apex.sh" "$stage"
for name in matonos-flatpak matonos-bwrap matonos-app-exec; do
    printf '%s %s bytes\n' "$name" "$(wc -c < "$stage/$name" | tr -d ' ')"
done

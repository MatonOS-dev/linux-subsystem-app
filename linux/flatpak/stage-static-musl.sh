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
mkdir -p "$helpers/linux/flatpak" "$helpers/linux/dbus-broker" "$helpers/install/linuxd"
install -m644 "$TREE/linux/flatpak/flatpak-env-wrapper.c" "$helpers/linux/flatpak/flatpak-env-wrapper.c"
install -m644 "$TREE/linux/flatpak/machine-id.h" "$helpers/linux/flatpak/machine-id.h"
install -m644 "$TREE/linux/flatpak/app-session.h" "$helpers/linux/flatpak/app-session.h"
install -m644 "$TREE/linux/flatpak/socket-relay.h" "$helpers/linux/flatpak/socket-relay.h"
install -m644 "$TREE/linux/dbus-broker/session-control.h" "$helpers/linux/dbus-broker/session-control.h"
install -m644 "$TREE/install/linuxd/GtkSettings.h" "$helpers/install/linuxd/GtkSettings.h"
install -m644 "$TREE/install/linuxd/SafePath.h" "$helpers/install/linuxd/SafePath.h"
install -m644 "$TREE/install/linuxd/MatonMls.h" "$helpers/install/linuxd/MatonMls.h"
install -m644 "$TREE/install/linuxd/FlatpakStore.c" "$helpers/install/linuxd/FlatpakStore.c"
install -m644 "$TREE/install/linuxd/FlatpakStore.h" "$helpers/install/linuxd/FlatpakStore.h"
(
    cd "$APEX_REPO/flatpak"
    find helpers -type f ! -name SOURCE.sha256 -print0 | sort -z | xargs -0 sha256sum
) > "$helpers/SOURCE.sha256"

ARCH=$ARCH HELPERS_ONLY=1 "$APEX_REPO/flatpak/alpine-enter.sh" /work/build-helpers.sh

out=/mnt/data/aosp/out/pc-logs/musl-324/output
stage="$TREE/linux/flatpak/prebuilt/static/$ARCH"
mkdir -p "$stage"
for name in matonos-flatpak matonos-bwrap matonos-app-exec flatpak-env-wrapper matonos-flatpak-store; do
    install -m755 "$out/$name" "$stage/$name"
done

license_src="$APEX_REPO/flatpak/licenses"
license_dst="$TREE/linux/flatpak/licenses"
[ -s "$license_src/NOTICE" ] && [ -s "$license_src/linked-components.json" ] || {
    echo "ERROR: generated Flatpak license records are missing; run the gated static build first" >&2; exit 1;
}
rm -rf "$license_dst"
mkdir -p "$license_dst"
cp "$APEX_REPO/flatpak/LICENSES.md" "$license_dst/LICENSES.md"
cp "$license_src/NOTICE" "$license_src/linked-components.json" "$license_dst/"
find "$license_src" -maxdepth 1 -type f -name '*.txt' -exec cp '{}' "$license_dst/" \;

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
    printf '\nflatpak-env-wrapper\n  SHA-256: %s\n  Sources: linux/flatpak/flatpak-env-wrapper.c and Linux session headers\n  Source SHA-256: %s\n  Toolchain: Alpine 3.24 musl, cc, -Os -ffunction-sections -fdata-sections -static-pie -Wl,--gc-sections; strip --strip-all\n' \
        "$(sha256sum "$stage/flatpak-env-wrapper" | awk '{print $1}')" \
        "$(sha256sum "$TREE/linux/flatpak/flatpak-env-wrapper.c" | awk '{print $1}')"
    printf '\nmatonos-flatpak-store\n  SHA-256: %s\n  Sources: install/linuxd/FlatpakStore.c and FlatpakStore.h\n  Source SHA-256: %s\n  Header SHA-256: %s\n  Toolchain: Alpine 3.24 musl, cc, -Os -ffunction-sections -fdata-sections -static-pie -Wl,--gc-sections; strip --strip-all\n' \
        "$(sha256sum "$stage/matonos-flatpak-store" | awk '{print $1}')" \
        "$(sha256sum "$TREE/install/linuxd/FlatpakStore.c" | awk '{print $1}')" \
        "$(sha256sum "$TREE/install/linuxd/FlatpakStore.h" | awk '{print $1}')"
    printf '\nAPEX license notices and per-component license texts\n'
    (cd "$license_dst" && find . -type f ! -name SOURCE.sha256 -print0 | sort -z | xargs -0 sha256sum)
} > "$source_file"
rm -f "$source_file.tmp"
"$APEX_REPO/flatpak/check-static-apex.sh" "$stage"
for name in matonos-flatpak matonos-bwrap matonos-app-exec flatpak-env-wrapper matonos-flatpak-store; do
    printf '%s %s bytes\n' "$name" "$(wc -c < "$stage/$name" | tr -d ' ')"
done

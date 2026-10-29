#!/usr/bin/env bash
# Recreate the offline Alpine musl build root and pinned Flatpak source cache.
set -Eeuo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
SCRATCH=${MUSL_SCRATCH:-$HOME/matonos/build/musl-324}
ROOTFS=$SCRATCH/rootfs/x86_64
CACHE=$SCRATCH/flatpak-musl/cache
FLATPAK_WORKTREE=$SCRATCH/flatpak-work
ALPINE_VERSION=3.24.2
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/alpine-minirootfs-${ALPINE_VERSION}-x86_64.tar.gz"
ALPINE_SHA256=c5ca053cfe1d85c5b96dff8b9bc57045f7f184a30ffb6b65776409ca90388677
FLATPAK_COMMIT=8078df1477d6b6f1c15efaf327889a52a8adfedc

for tool in bwrap curl git sha256sum tar; do
  command -v "$tool" >/dev/null || { echo "Required host tool missing: $tool" >&2; exit 2; }
done
BWRAP=$(command -v bwrap)
mkdir -p "$SCRATCH" "$CACHE"

if [[ ! -f $SCRATCH/.scratch-ready || ! -x $ROOTFS/bin/sh ]]; then
  rm -rf "$ROOTFS"
fi
if [[ ! -x $ROOTFS/bin/sh ]]; then
  archive=$SCRATCH/alpine-minirootfs-${ALPINE_VERSION}-x86_64.tar.gz
  if [[ ! -s $archive ]] || ! printf '%s  %s\n' "$ALPINE_SHA256" "$archive" | sha256sum -c - >/dev/null 2>&1; then
    rm -f "$archive"
    curl -fL "$ALPINE_URL" -o "$archive"
  fi
  printf '%s  %s\n' "$ALPINE_SHA256" "$archive" | sha256sum -c -
  mkdir -p "$ROOTFS"
  tar -xzf "$archive" -C "$ROOTFS"
fi

# Keep both repositories on the same immutable Alpine 3.24 package branch.
cat > "$ROOTFS/etc/apk/repositories" <<'REPOSITORIES'
https://dl-cdn.alpinelinux.org/alpine/v3.24/main
https://dl-cdn.alpinelinux.org/alpine/v3.24/community
REPOSITORIES
# The minirootfs has no resolver file. Use the host's real upstream resolver
# addresses so the isolated setup namespace can reach the Alpine mirrors.
if [[ -r /run/systemd/resolve/resolv.conf ]]; then
  install -m644 /run/systemd/resolve/resolv.conf "$ROOTFS/etc/resolv.conf"
else
  install -m644 /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
fi

# These cover the pinned Meson/Ninja recipes, autotools bootstraps, static
# native builds, EROFS configure step, source tools, and bubblewrap's libcap.
PACKAGES=(
  autoconf automake bash bison build-base cmake curl file flex gettext-dev gettext-static git
  gperf gzip libcap-dev libtool linux-headers meson ninja patch perl pkgconf
  py3-parsing python3 tar xz zlib-dev zlib-static zstd
)
"$BWRAP" \
  --bind "$ROOTFS" / \
  --proc /proc --dev /dev --tmpfs /tmp \
  --unshare-user --unshare-ipc --unshare-pid --uid 0 \
  --setenv PATH /usr/bin:/usr/sbin:/bin:/sbin \
  --setenv HOME /root \
  /bin/sh -ec 'apk update; apk --no-chown add --no-cache "$@"' setup-scratch "${PACKAGES[@]}" \
  > "$SCRATCH/apk-install.log" 2>&1 || {
    tail -n 80 "$SCRATCH/apk-install.log" >&2
    exit 1
  }

# alpine-enter.sh binds this checkout at its stable /scratch/flatpak-work path.
if [[ ! -d $FLATPAK_WORKTREE/.git ]]; then
  git clone --no-checkout https://github.com/MatonOS-dev/flatpak "$FLATPAK_WORKTREE"
fi
git -C "$FLATPAK_WORKTREE" fetch --no-tags origin "$FLATPAK_COMMIT"
git -C "$FLATPAK_WORKTREE" checkout --detach --force "$FLATPAK_COMMIT"
actual=$(git -C "$FLATPAK_WORKTREE" rev-parse HEAD)
[[ $actual == "$FLATPAK_COMMIT" ]] || { echo "Flatpak checkout mismatch: $actual" >&2; exit 1; }

CACHE_DIR=$CACHE sh "$HERE/fetch.sh"
grep -q $'flatpak\t8078df1477d6b6f1c15efaf327889a52a8adfedc\t' "$CACHE/src/flatpak.pin"
touch "$SCRATCH/.scratch-ready"
printf 'Alpine %s scratch ready: %s\nFlatpak source cache ready: %s\n' \
  "$ALPINE_VERSION" "$ROOTFS" "$CACHE"

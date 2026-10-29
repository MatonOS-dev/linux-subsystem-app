#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCH=${ARCH:-$(uname -m)}
SCRATCH=${SCRATCH:-$HOME/matonos/build/musl-324}
FLATPAK_MUSL_WORK=${FLATPAK_MUSL_WORK:-$SCRATCH/flatpak-musl}
FLATPAK_CACHE=${FLATPAK_CACHE:-$SCRATCH/flatpak-musl/cache}
ROOTFS=$SCRATCH/rootfs/$ARCH
GPGME_LITE_SRC=${GPGME_LITE_SRC:-$(CDPATH= cd -- "$HERE/../../DullPGP" && pwd)}
[ -x "$ROOTFS/bin/sh" ] || { echo "Missing offline Alpine build root: $ROOTFS" >&2; exit 1; }
[ -f "$GPGME_LITE_SRC/Makefile" ] || { echo "Missing DullPGP source tree: $GPGME_LITE_SRC" >&2; exit 1; }
mkdir -p "$FLATPAK_MUSL_WORK" "$FLATPAK_CACHE"
exec /usr/bin/bwrap \
  --bind "$ROOTFS" / \
  --bind "$SCRATCH" /scratch \
  --bind "$FLATPAK_MUSL_WORK" /scratch/work \
  --bind "$FLATPAK_CACHE" /scratch/work/cache \
  --bind "$HERE" /work \
  --bind "$GPGME_LITE_SRC" /work-gpgme-lite \
  --proc /proc --dev /dev --tmpfs /tmp \
  --unshare-net --unshare-user --unshare-ipc --unshare-pid --uid 0 \
  --setenv PATH /usr/bin:/usr/sbin:/bin:/sbin \
  --setenv HOME /root \
  --setenv ARCH "$ARCH" \
  --setenv FLATPAK_REPO /scratch/flatpak-work \
  --setenv FLATPAK_COMMIT 03e6b205d01c9560a829941d95065a7c683d9667 \
  "$@"

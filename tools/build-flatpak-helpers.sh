#!/usr/bin/env bash
# Rebuild the in-tree Flatpak launch helpers that ship as APK inputs in
# linux/flatpak/prebuilt/static/<arch>/ when sources or the recipe changed.
# build-apps.sh packages these beside the v3 loader; MATON_FORCE_HELPERS=1
# rebuilds unconditionally.
#
# The Flatpak/OSTree multicall and portal come from MatonOS_apexs;
# the unprivileged applet entries are rebuilt here.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
ARCH=${ARCH:-x86_64}

MUSL_SCRATCH=${MUSL_SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
GPGME_LITE_SRC=${GPGME_LITE_SRC:-$MUSL_SCRATCH/flatpak-musl/cache/src/DullPGP}
ALPINE_ENTER=$DEVICE_DIR/linux/flatpak/build-support/alpine-enter.sh
[[ -x $MUSL_SCRATCH/rootfs/x86_64/bin/sh ]] || die "offline Alpine 3.24.2 root missing at $MUSL_SCRATCH"
CFLAGS='-Os -march=x86-64-v2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -static-pie -static'
MUSL_CC_ID=$(SCRATCH="$MUSL_SCRATCH" GPGME_LITE_SRC="$GPGME_LITE_SRC" "$ALPINE_ENTER" gcc --version | sed -n '1p')

SRC=$DEVICE_DIR/linux/flatpak
STAGE=$SRC/prebuilt/static/$ARCH
STAMPS=$STAGE/HELPERS.sha256

# source -> installed names (one binary may be installed under several).
declare -A HELPERS=(
  [matonos-app-tool]="matonos-app-flatpak"
)

source_file() {
  printf '%s\n' "$SRC/$1.c"
}

# Fingerprint: compiler, flags, and every source/header the compiler reads.
fingerprint() {
  local src
  src=$(source_file "$1")
  {
    printf '%s\n' "$MUSL_CC_ID" "$CFLAGS"
    sha256sum "$src" | awk '{print $1}'
  } | sha256sum | awk '{print $1}'
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
touch "$STAMPS"
changed=0
for name in $(printf '%s\n' "${!HELPERS[@]}" | sort); do
  fp=$(fingerprint "$name")
  installed_ok=1
  for out in ${HELPERS[$name]}; do [[ -x $STAGE/$out ]] || installed_ok=0; done
  if [[ -z ${MATON_FORCE_HELPERS:-} && $installed_ok == 1 ]] &&
     grep -qx "$fp  $name" "$STAMPS"; then
    continue
  fi
  info "Flatpak helper $name (sources changed)"
  helper_work=${MATON_FLATPAK_HELPER_WORK:-${FLATPAK_MUSL_WORK:-$MUSL_SCRATCH/flatpak-musl}/helper-build}
  mkdir -p "$helper_work"
  install -m644 "$(source_file "$name")" "$helper_work/$name.c"
  SCRATCH="$MUSL_SCRATCH" FLATPAK_MUSL_WORK="${FLATPAK_MUSL_WORK:-$MUSL_SCRATCH/flatpak-musl}" GPGME_LITE_SRC="$GPGME_LITE_SRC" "$ALPINE_ENTER" sh -c \
    "gcc $CFLAGS /scratch/work/helper-build/$name.c -o /scratch/work/helper-build/$name"
  if readelf -l "$helper_work/$name" | grep -q 'INTERP'; then die "$name unexpectedly has PT_INTERP"; fi
  install -m755 "$helper_work/$name" "$tmp/$name"
  for out in ${HELPERS[$name]}; do install -m755 "$tmp/$name" "$STAGE/$out"; done
  { grep -v "  $name\$" "$STAMPS" || true; echo "$fp  $name"; } | sort -k2 > "$tmp/stamps"
  cp "$tmp/stamps" "$STAMPS"
  changed=1
done
[[ $changed == 1 ]] || info "Flatpak helpers up to date"

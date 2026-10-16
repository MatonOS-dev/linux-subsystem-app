#!/usr/bin/env bash
# Rebuild the in-tree Flatpak launch helpers that ship as prebuilts in the
# com.matonos.flatpak APEX (linux/flatpak/prebuilt/static/<arch>/), but only
# when their sources or the recipe changed. Soong then repacks the APEX
# because its prebuilt inputs changed. build.sh runs this before the AOSP
# build; MATON_FORCE_HELPERS=1 rebuilds unconditionally.
#
# The Flatpak/OSTree multicall and portal come from MatonOS_apexs;
# the unprivileged applet entries are rebuilt here.
set -Eeuo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }
DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
. "$(dirname -- "$(readlink -f -- "$0")")/local-env.sh"
NDK=${ANDROID_NDK:-$HOME/Documents/android-ndk-r30}
API=36
ARCH=x86_64
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/$ARCH-linux-android$API-clang
[[ -x $CC ]] || die "NDK r30/API $API missing at $NDK"
CFLAGS=(-Os -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections)

SRC=$DEVICE_DIR/linux/flatpak
STAGE=$SRC/prebuilt/static/$ARCH
STAMPS=$STAGE/HELPERS.sha256

# source -> installed names (one binary may be installed under several).
declare -A HELPERS=(
  [matonos-app-tool]="matonos-app-flatpak matonos-app-bwrap"
)

source_file() {
  printf '%s\n' "$SRC/$1.c"
}

# Fingerprint: compiler, flags, and every source/header the compiler reads.
fingerprint() {
  local src
  src=$(source_file "$1")
  {
    "$CC" --version | head -1
    printf '%s\n' "${CFLAGS[@]}"
    "$CC" -MM -MT x "$src" | tr -d '\\\n' | tr ' ' '\n' | sed '/^x:$/d;/^$/d' | sort -u |
      while read -r f; do sha256sum "$f" | awk '{print $1}'; done
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
  "$CC" "${CFLAGS[@]}" "$(source_file "$name")" -o "$tmp/$name"
  for out in ${HELPERS[$name]}; do install -m755 "$tmp/$name" "$STAGE/$out"; done
  { grep -v "  $name\$" "$STAMPS" || true; echo "$fp  $name"; } | sort -k2 > "$tmp/stamps"
  cp "$tmp/stamps" "$STAMPS"
  changed=1
done
[[ $changed == 1 ]] || info "Flatpak helpers up to date"

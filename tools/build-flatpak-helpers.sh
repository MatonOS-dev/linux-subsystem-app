#!/usr/bin/env bash
# Rebuild the in-tree Flatpak launch helpers that ship as prebuilts in the
# com.matonos.flatpak APEX (linux/flatpak/prebuilt/static/<arch>/), but only
# when their sources or the recipe changed. Soong then repacks the APEX
# because its prebuilt inputs changed. build.sh runs this before the AOSP
# build; MATON_FORCE_HELPERS=1 rebuilds unconditionally.
#
# Also builds the SysV shm LD_PRELOAD helper (linux/sysvipc/shm-helper.c) for
# each glibc ABI the APEX serves; matonos-bwrap injects it into app sandboxes.
# The Flatpak/OSTree multicall, the portal, matonos-app-exec and
# matonos-flatpak-store come from MatonOS_apexs and are not rebuilt here.
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
  [flatpak-env-wrapper]="flatpak-env-wrapper"
  [matonos-bwrap]="matonos-bwrap"
  [matonos-app-tool]="matonos-app-flatpak matonos-app-bwrap"
)

# Fingerprint: compiler, flags, and every source/header the compiler reads.
fingerprint() {
  local src=$SRC/$1.c
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
  "$CC" "${CFLAGS[@]}" "$SRC/$name.c" -o "$tmp/$name"
  for out in ${HELPERS[$name]}; do install -m755 "$tmp/$name" "$STAGE/$out"; done
  { grep -v "  $name\$" "$STAMPS" || true; echo "$fp  $name"; } | sort -k2 > "$tmp/stamps"
  cp "$tmp/stamps" "$STAMPS"
  changed=1
done
# Freestanding glibc preload libraries: no libc headers, so one clang builds
# every ABI; symbols bind to the runtime's glibc at load time.
CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
PRELOAD_FLAGS=(-O2 -fPIC -shared -ffreestanding -nostdlib -fuse-ld=lld -Wall -Wextra -Werror)
SHM_SRC=$DEVICE_DIR/linux/sysvipc
for pair in x86_64:x86_64-linux-gnu i386:i686-linux-gnu; do
  name=libmaton-shm-${pair%%:*}.so target=${pair#*:}
  fp=$({ "$CLANG" --version | head -1; printf '%s\n' "$target" "${PRELOAD_FLAGS[@]}"
         sha256sum "$SHM_SRC/shm-helper.c" "$SHM_SRC/sysvipc.h" | awk '{print $1}'; } | sha256sum | awk '{print $1}')
  if [[ -z ${MATON_FORCE_HELPERS:-} && -s $STAGE/$name ]] && grep -qx "$fp  $name" "$STAMPS"; then
    continue
  fi
  info "SysV shm helper $name (sources changed)"
  "$CLANG" --target="$target" "${PRELOAD_FLAGS[@]}" "$SHM_SRC/shm-helper.c" -o "$tmp/$name"
  install -m644 "$tmp/$name" "$STAGE/$name"
  { grep -v "  $name\$" "$STAMPS" || true; echo "$fp  $name"; } | sort -k2 > "$tmp/stamps"
  cp "$tmp/stamps" "$STAMPS"
  changed=1
done
[[ $changed == 1 ]] || info "Flatpak helpers up to date"

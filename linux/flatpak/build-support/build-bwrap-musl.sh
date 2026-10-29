#!/usr/bin/env bash
# Build the pinned Flatpak bubblewrap source as a static musl executable.
set -Eeuo pipefail
die() { echo "ERROR: $*" >&2; exit 1; }
ROOTFS=${MUSL_ROOTFS:-$HOME/matonos/build/musl-324/rootfs/x86_64}
SOURCE=${BUBBLEWRAP_SOURCE:?Set BUBBLEWRAP_SOURCE to the pinned bubblewrap 0.13.0 source tree}
OUTPUT=${BWRAP_OUTPUT:?Set BWRAP_OUTPUT to the output binary path}
WORK=${BWRAP_WORK:-${OUTPUT}.build}
[[ -x $ROOTFS/bin/sh && -x $ROOTFS/usr/bin/meson && -x $ROOTFS/usr/bin/ninja ]] || die "musl build root is incomplete: $ROOTFS"
[[ -f $SOURCE/meson.build && -f $ROOTFS/usr/lib/pkgconfig/libcap.pc ]] || die "bubblewrap source or musl libcap development files are missing"
mkdir -p "$WORK"
work=$(realpath "$WORK")
output=$(realpath -m "$OUTPUT")
mkdir -p "$(dirname "$output")"
source=$(realpath "$SOURCE")
output_name=$(basename "$output")
/usr/bin/bwrap \
  --bind "$ROOTFS" / \
  --bind "$source" /src \
  --bind "$work" /work \
  --bind "$(dirname "$output")" /out \
  --proc /proc --dev /dev --tmpfs /tmp \
  --unshare-net --unshare-user --unshare-ipc --unshare-pid --uid 0 \
  --setenv PATH /usr/bin:/usr/sbin:/bin:/sbin \
  --setenv HOME /root --setenv LC_ALL C --setenv SOURCE_DATE_EPOCH 1767225600 \
  sh -eu -s -- "$output_name" <<'BUILD_SCRIPT'
rm -rf /work/build /work/source
cp -a /src /work/source
sed -i "s|libcap_dep = dependency.*|libcap_dep = dependency('libcap', required : true, static : true)|" /work/source/meson.build
meson setup /work/build /work/source --prefix=/usr --buildtype=release \
  --default-library=static -Dtests=false -Dman=disabled \
  -Dselinux=disabled -Dbash_completion=disabled -Dzsh_completion=disabled \
  -Dc_link_args="-static -Wl,--build-id=none"
ninja -C /work/build
install -m 0755 /work/build/bwrap "/out/$1"
BUILD_SCRIPT
file "$output" | grep -Eq 'static(-pie)? linked' || die "output is not static: $output"
if readelf -l "$output" | grep -q 'INTERP'; then die "output unexpectedly has an ELF interpreter"; fi
echo "Built static musl bubblewrap: $output"

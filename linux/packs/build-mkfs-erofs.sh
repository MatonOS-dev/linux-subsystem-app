#!/usr/bin/env bash
set -Eeuo pipefail
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SCRATCH=${EROFS_MUSL_SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
WORK=${EROFS_MUSL_WORK:-/mnt/data/aosp/out/pc-logs/erofs-pipeline}
ROOTFS=$SCRATCH/rootfs/x86_64
ARCHIVE=$WORK/src/erofs-utils-v1.9.4.tar.gz
SOURCE=$WORK/src/erofs-utils-1.9.4
BUILD=$WORK/build/erofs-utils-1.9.4
OUT=$WORK/out/matonos-mkfs-erofs
PREBUILT=$HERE/prebuilt/x86_64/matonos-mkfs-erofs
[[ -x $ROOTFS/bin/sh ]] || { echo "Missing Alpine musl rootfs: $ROOTFS" >&2; exit 1; }
mkdir -p "$WORK/src" "$WORK/build" "$WORK/out" "$(dirname "$PREBUILT")"
cat > "$WORK/build/static-cc" <<'EOF'
#!/bin/sh
exec gcc "$@" -static
EOF
chmod 0755 "$WORK/build/static-cc"
if [[ ! -s $ARCHIVE ]]; then
  curl -fL https://github.com/erofs/erofs-utils/archive/refs/tags/v1.9.4.tar.gz -o "$ARCHIVE"
fi
printf '%s  %s\n' 7d135aa2550326a5acf20f53c518aea5a8900015ce50700044e40f818c31dd80 "$ARCHIVE" | sha256sum -c -
rm -rf "$SOURCE" "$BUILD"
mkdir -p "$SOURCE" "$BUILD"
tar -xzf "$ARCHIVE" -C "$SOURCE" --strip-components=1
chmod -R u+w "$SOURCE"
/usr/bin/bwrap \
  --bind "$ROOTFS" / \
  --bind "$WORK" /scratch \
  --bind "$SOURCE" /source \
  --bind "$HERE" /recipe \
  --proc /proc --dev /dev --tmpfs /tmp \
  --unshare-net --unshare-user --unshare-ipc --unshare-pid --uid 0 \
  --setenv PATH /usr/bin:/usr/sbin:/bin:/sbin \
  --setenv CC /scratch/build/static-cc \
  --setenv HOME /root --setenv SOURCE_DATE_EPOCH 0 \
  /bin/sh -ec '
    cd /source
    ./autogen.sh
    CFLAGS="-O2 -g0 -ffile-prefix-map=/source=./src -fdebug-prefix-map=/source=./src" \
      ./configure --prefix=/usr --enable-lz4 --disable-lzma --disable-fuse \
        --without-zlib --without-libzstd --without-libcurl --without-libxml2 --without-uuid --without-libnl3
    make -j4
    cp mkfs/mkfs.erofs /scratch/out/matonos-mkfs-erofs
  '
install -m 0755 "$OUT" "$PREBUILT"
if readelf -l "$PREBUILT" | grep -q INTERP; then
  echo "mkfs.erofs unexpectedly has a dynamic interpreter" >&2
  exit 1
fi
"$PREBUILT" --version
(cd "$(dirname "$PREBUILT")" && sha256sum "$(basename "$PREBUILT")") > "$HERE/prebuilt/x86_64/SHA256SUMS"
echo "Built static musl mkfs.erofs: $PREBUILT"

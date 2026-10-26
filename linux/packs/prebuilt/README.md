# Android EROFS packer executable

`x86_64/matonos-mkfs-erofs` and `x86_64/matonos-fsck-erofs` are x86_64
static-musl executables packaged as executable JNI libraries in the host APK.
They are launched as separate programs; neither is linked into MatonOS code.
The checker and image writer come from the same pinned source build. Rebuild with
`bash linux/packs/build-mkfs-erofs.sh`. The recipe uses the existing Alpine
3.24.2 x86_64 toolchain (GCC 15.2.0, musl 1.2.6-r2), erofs-utils v1.9.4
source archive SHA-256
`7d135aa2550326a5acf20f53c518aea5a8900015ce50700044e40f818c31dd80`, and
the existing static LZ4 1.10.0. Deployment images use LZ4HC. The binary
uses erofs-utils files under GPL-2.0-or-later and MIT; the selected binary
license is GPL-2.0-or-later. License texts are in `../licenses/` and the
source archive is available at the pinned upstream URL used by the build
recipe.

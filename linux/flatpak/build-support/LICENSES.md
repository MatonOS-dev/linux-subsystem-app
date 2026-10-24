# MatonOS Flatpak APEX licenses

This bundle statically links the components listed in `licenses/linked-components.json`.
The selected license option for every SPDX alternative is recorded there. Components with GPL-3.0 or LGPL-3.0 as the only usable option are rejected by the build gate.

The `matonos-flatpak` multicall also exposes Flatpak's upstream `portal` entry
point (`org.freedesktop.portal.Flatpak`). That implementation is part of the
pinned Flatpak source and is licensed LGPL-2.1-or-later; it shares the Flatpak
library and dependency set already recorded for the multicall binary.

For LGPL-2.1 components, MatonOS provides the corresponding source through the pinned inputs in `flatpak/sources.lock` (upstream commits and release archives, and the listed forks). Users can relink the binaries by running `flatpak/fetch.sh` and `flatpak/build.sh`; the build regenerates the binaries and the `SOURCE` manifest.

See `licenses/NOTICE` and the per-component license texts in `licenses/`.

`linux/packs/prebuilt/x86_64/matonos-mkfs-erofs` is a separate, statically
linked executable built from the pinned erofs-utils v1.9.4 source archive.
It is launched with `exec` by the installer and is not linked into MatonOS
code. erofs-utils is GPL-2.0-or-later with MIT-licensed files; the license
gate records the GPL-2.0-or-later option. Source and license texts are in
`linux/packs/licenses/`, with the archive pin and rebuild recipe in
`linux/packs/prebuilt/README.md` and `linux/packs/build-mkfs-erofs.sh`.

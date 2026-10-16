> v3.5: linuxd is retired. Any linuxd/shared-store paths below describe the
> historical design. See [current ownership and device checks](../../../../docs/RETIRE-LINUXD.md).

# Bionic Flatpak tool inputs

The directory name is retained for build-input stability. The payload is bionic.
`tools/build-flatpak-helpers.sh` builds matonos-app-tool (installed as
matonos-app-flatpak and matonos-app-bwrap). HELPERS.sha256 fingerprints their
compiler, flags and included sources. SOURCE records upstream multicall/portal
provenance; later in-tree artifact hashes supersede the original helper hashes.

Build fork commit `4cce363` in `~/matonos/repos/flatpak` using
`~/matonos/repos/MatonOS_apexs/flatpak/build.sh`; copy its generated
`flatpak/out/matonos-flatpak` into this directory before building the APK.
This packaging move is v3.5.
The old bwrap/app-exec binaries, the Flatpak portal (v1/v2) and the SysV
preload libraries are deleted.
No fallback launch artifacts are shipped.
